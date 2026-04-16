#include "rdr_signaling.hpp"

#include "h264_annexb_decoder.hpp"
#include "h264_d3d11va_decoder.hpp"
#include "h264_decoder_iface.hpp"
#include "receiver_context.hpp"
#include "rdr_log.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <variant>
#include <vector>

#include <rtc/rtc.hpp>
#include <rtc/h264rtpdepacketizer.hpp>

#include <nlohmann/json.hpp>

#pragma comment(lib, "ws2_32.lib")

using json = nlohmann::json;

static void EnsureWinsockOnce() {
	static std::once_flag once;
	std::call_once(once, [] {
		WSADATA wsa{};
		const int r = WSAStartup(MAKEWORD(2, 2), &wsa);
		if (r != 0) Logf("[net] WSAStartup failed: %d\n", r);
	});
}

static double SmoothEwma(std::optional<double>& prev, double sample, double alpha) {
	if (!prev.has_value()) prev = sample;
	else prev = prev.value() * (1.0 - alpha) + sample * alpha;
	return prev.value();
}

// Custom per-frame telemetry carried in H264 SEI (user_data_unregistered).
// Layout (rbsp user_data_unregistered):
//   payloadType=5, payloadSize >= 16(uuid)+1(version)+8(frameId)+8(capMs)+8(encMs)+8(sendMs)
// We extract all 4 agent timestamps so receiver can segment delays.
static bool ExtractRpcLatencySeiFieldsFromAnnexB(
	const uint8_t* buf,
	size_t len,
	uint64_t& outFrameId,
	uint64_t& outCapMs,
	uint64_t& outEncMs,
	uint64_t& outSendMs
) {
	static constexpr std::array<uint8_t, 16> kUuid = {
		0x52, 0x50, 0x43, 0x2D, 0x4C, 0x41, 0x54, 0x45,
		0x4E, 0x43, 0x59, 0x2D, 0x53, 0x45, 0x49, 0x31
	}; // "RPC-LATENCY-SEI1"

	auto findStartCode = [&](size_t from, size_t& scPos, size_t& scLen) -> bool {
		for (size_t i = from; i + 3 <= len; ++i) {
			if (buf[i] == 0x00 && buf[i + 1] == 0x00) {
				if (buf[i + 2] == 0x01) { scPos = i; scLen = 3; return true; }
				if (i + 3 < len && buf[i + 2] == 0x00 && buf[i + 3] == 0x01) { scPos = i; scLen = 4; return true; }
			}
		}
		return false;
	};

	auto ebspToRbsp = [&](const uint8_t* ebsp, size_t ebspLen, std::vector<uint8_t>& rbsp) {
		rbsp.clear();
		rbsp.reserve(ebspLen);
		int zeroCount = 0;
		for (size_t i = 0; i < ebspLen; ++i) {
			const uint8_t b = ebsp[i];
			if (zeroCount == 2 && b == 0x03) {
				zeroCount = 0;
				continue;
			}
			rbsp.push_back(b);
			if (b == 0x00) zeroCount++;
			else zeroCount = 0;
		}
	};

	size_t off = 0;
	size_t scPos = 0, scLen = 0;
	while (findStartCode(off, scPos, scLen)) {
		const size_t nalStart = scPos + scLen;
		if (nalStart >= len) break;
		size_t nextPos = 0, nextLen = 0;
		size_t nalEnd = len;
		if (findStartCode(nalStart, nextPos, nextLen)) nalEnd = nextPos;
		if (nalEnd <= nalStart) { off = nalStart; continue; }

		const uint8_t nalHdr = buf[nalStart];
		const uint8_t nalType = (uint8_t)(nalHdr & 0x1F);
		if (nalType == 6) {
			const uint8_t* ebsp = buf + nalStart + 1;
			const size_t ebspLen = (nalEnd > nalStart + 1) ? (nalEnd - (nalStart + 1)) : 0;
			std::vector<uint8_t> rbsp;
			ebspToRbsp(ebsp, ebspLen, rbsp);
			size_t pos = 0;
			while (pos < rbsp.size()) {
				uint32_t payloadType = 0;
				while (pos < rbsp.size() && rbsp[pos] == 0xFF) { payloadType += 255; pos++; }
				if (pos >= rbsp.size()) break;
				payloadType += rbsp[pos++];

				uint32_t payloadSize = 0;
				while (pos < rbsp.size() && rbsp[pos] == 0xFF) { payloadSize += 255; pos++; }
				if (pos >= rbsp.size()) break;
				payloadSize += rbsp[pos++];

				if (pos + payloadSize > rbsp.size()) break;
				// payloadType=5: user_data_unregistered
				// uuid(16)+version(1)+frameId(8)+capMs(8)+encMs(8)+sendMs(8) => 49 bytes
				if (payloadType == 5 && payloadSize >= 49) {
					const uint8_t* payload = rbsp.data() + pos;
					bool uuidOk = true;
					for (size_t i = 0; i < 16; ++i) {
						if (payload[i] != kUuid[i]) { uuidOk = false; break; }
					}
					if (uuidOk) {
						// payload[16] = version (currently unused)
						uint64_t frameId = 0;
						uint64_t capMs = 0;
						uint64_t encMs = 0;
						uint64_t sendMs = 0;

						// little-endian uint64s
						for (int i = 0; i < 8; ++i) frameId |= (uint64_t)payload[17 + i] << (8 * i);
						for (int i = 0; i < 8; ++i) capMs   |= (uint64_t)payload[25 + i] << (8 * i);
						for (int i = 0; i < 8; ++i) encMs   |= (uint64_t)payload[33 + i] << (8 * i);
						for (int i = 0; i < 8; ++i) sendMs  |= (uint64_t)payload[41 + i] << (8 * i);

						outFrameId = frameId;
						outCapMs = capMs;
						outEncMs = encMs;
						outSendMs = sendMs;
						return capMs != 0 && encMs != 0 && sendMs != 0;
					}
				}
				pos += payloadSize;
			}
		}

		off = nalEnd;
	}

	return false;
}

static void StartLatencyPingThread() {
	if (g_pingThreadStarted.exchange(true, std::memory_order_acq_rel)) {
		return;
	}
	std::thread([] {
		uint64_t pingSeq = 0;
		while (!g_exitRequested.load(std::memory_order_relaxed)) {
			std::shared_ptr<rtc::DataChannel> dc;
			{
				std::lock_guard<std::recursive_mutex> lk(g_dcMtx);
				dc = g_dataChannel;
			}
			if (!dc || !dc->isOpen()) {
				std::this_thread::sleep_for(std::chrono::milliseconds(200));
				continue;
			}

			const uint64_t tCli = SystemMsNow();
			pingSeq++;
			json j = { {"type", "latPing"}, {"seq", pingSeq}, {"tCli", (int64_t)tCli} };
			SendToDataChannelAsync(j.dump());

			std::this_thread::sleep_for(std::chrono::milliseconds(3000));
		}
		g_pingThreadStarted.store(false, std::memory_order_release);
	}).detach();
}

static void HandleDataChannelMessage(const std::string& msg) {
	if (msg.find("Ping") != std::string::npos) {
		static std::atomic<uint64_t> lastPongMs{0};
		const uint64_t now = SystemMsNow();
		uint64_t expected = lastPongMs.load(std::memory_order_relaxed);
		if (expected == 0 || (now >= expected + 10000)) {
			if (lastPongMs.compare_exchange_strong(
				expected, now,
				std::memory_order_relaxed, std::memory_order_relaxed)) {
				SendToDataChannelAsync("Pong " + std::to_string(now));
			}
		}
		return;
	}

	json j;
	try {
		j = json::parse(msg);
	} catch (...) {
		return;
	}

	const std::string type = j.value("type", "");
	if (type == "latPong") {
		const uint64_t tCli = (uint64_t)j.value("tCli", (int64_t)0);
		const uint64_t tSrv = (uint64_t)j.value("tSrv", (int64_t)0);
		const uint64_t t2 = SystemMsNow();
		const uint64_t rtt = (t2 >= tCli) ? (t2 - tCli) : 0;
		const double theta = ((double)tSrv + (double)rtt / 2.0) - (double)t2;
		{
			std::lock_guard<std::mutex> lk(g_latency.mtx);
			SmoothEwma(g_latency.thetaMs, theta, 0.25);
		}
		return;
	}

	// frameMark is deprecated: we now embed timestamps in per-frame H264 SEI.
	if (type == "frameMark") {
		return;
	}

	if (type == "controlGranted") {
		g_controlEnabled.store(true, std::memory_order_relaxed);
		return;
	}
	if (type == "controlDenied" || type == "controlRevoked") {
		g_controlEnabled.store(false, std::memory_order_relaxed);
		return;
	}

	if (type == "remoteProcessExited") {
		Logf("[receiver] remoteProcessExited received\n");
		g_exitRequested.store(true, std::memory_order_relaxed);
		g_inputArmed.store(false, std::memory_order_relaxed);
		if (g_hwnd) PostMessage(g_hwnd, WM_CLOSE, 0, 0);
		return;
	}
}

static void SetupDataChannelCallbacks(const std::shared_ptr<rtc::PeerConnection>& pc) {
	pc->onDataChannel([](std::shared_ptr<rtc::DataChannel> dc) {
		{
			std::lock_guard<std::recursive_mutex> lk(g_dcMtx);
			g_dataChannel = dc;
		}

		dc->onOpen([dc] {
			(void)dc;
			g_controlEnabled.store(false, std::memory_order_relaxed);

			json req = { {"type", "controlRequest"} };
			SendToDataChannelAsync(req.dump());

			StartLatencyPingThread();
		});

		dc->onMessage(nullptr, [](std::string msg) {
			HandleDataChannelMessage(msg);
		});

		dc->onClosed([] {
			g_controlEnabled.store(false, std::memory_order_relaxed);
			g_videoTrackAttached.store(false, std::memory_order_relaxed);
			std::lock_guard<std::recursive_mutex> lk(g_dcMtx);
			g_dataChannel.reset();
			g_videoTrackKeepAlive.reset();
		});
	});
}

static std::shared_ptr<rtc::PeerConnection> SetupPeerConnectionForAnswerer(
	const std::shared_ptr<rtc::WebSocket>& ws,
	std::atomic<bool>& offerHandled,
	IH264ToBGRADecoder& decoder
) {
	rtc::Configuration config;
	config.disableAutoNegotiation = true;
	config.iceServers.emplace_back("stun:stun.l.google.com:19302");

	auto pc = std::make_shared<rtc::PeerConnection>(config);
	std::weak_ptr<rtc::PeerConnection> wpc = pc;

	pc->onGatheringStateChange([wpc, &offerHandled](rtc::PeerConnection::GatheringState st) {
		if (st != rtc::PeerConnection::GatheringState::Complete) return;
		Logf("[signaling] gathering complete\n");
		if (offerHandled.load(std::memory_order_relaxed)) return;
		auto pc2 = wpc.lock();
		if (!pc2) {
			Logf("[signaling] gathering complete but pc expired\n");
			return;
		}
		auto ldOpt = pc2->localDescription();
		if (!ldOpt.has_value()) {
			Logf("[signaling] gathering complete but no localDescription\n");
			return;
		}
		const rtc::Description ld = ldOpt.value();
		{
			int vport = -1;
			const std::string sdpTmp = std::string(ld);
			auto pos = sdpTmp.find("m=video ");
			if (pos != std::string::npos) {
				size_t i = pos + std::strlen("m=video ");
				while (i < sdpTmp.size() && (sdpTmp[i] < '0' || sdpTmp[i] > '9')) ++i;
				if (i < sdpTmp.size()) {
					size_t j = i;
					while (j < sdpTmp.size() && (sdpTmp[j] >= '0' && sdpTmp[j] <= '9')) ++j;
					if (j > i) vport = std::atoi(sdpTmp.substr(i, j - i).c_str());
				}
			}
			Logf("[signaling] gathering complete localDescription ended=%d m=video port=%d\n",
				(int)ld.ended(), vport);
		}
		std::string sdp = std::string(ld);
		size_t candCnt = 0;
		for (size_t p = sdp.find("a=candidate:"); p != std::string::npos; p = sdp.find("a=candidate:", p + 1)) {
			++candCnt;
		}
		auto FixRecvOnlyMLinePort = [&](const char* media) {
			const std::string from = std::string("m=") + media + " 0 ";
			const std::string to = std::string("m=") + media + " 9 ";
			size_t pos = 0;
			bool changed = false;
			while ((pos = sdp.find(from, pos)) != std::string::npos) {
				sdp.replace(pos, from.size(), to);
				pos += to.size();
				changed = true;
			}
			return changed;
		};

		const bool chV = FixRecvOnlyMLinePort("video");
		const bool chA = FixRecvOnlyMLinePort("audio");
		if (chV) Logf("[signaling] fixed SDP m=video 0 -> 9 (candidates=%llu)\n",
			(unsigned long long)candCnt);
		if (chA) Logf("[signaling] fixed SDP m=audio 0 -> 9 (candidates=%llu)\n",
			(unsigned long long)candCnt);
		if (!chV && !chA) {
			Logf("[signaling] SDP m-line ports already non-zero (candidates=%llu)\n",
				(unsigned long long)candCnt);
		}

		Logf("[signaling] sending SDP (candidates=%llu)\n",
			(unsigned long long)candCnt);

		json ans = { {"id", "server"}, {"type", "answer"}, {"sdp", sdp} };
		try {
			const std::string payload = ans.dump();
			{
				std::lock_guard<std::mutex> lk(g_wsSendPendingMtx);
				g_wsSendPending.push(payload);
			}
			Logf("[signaling] queued answer payload (gathering complete) bytes=%llu\n",
				(unsigned long long)payload.size());
			offerHandled.store(true, std::memory_order_relaxed);
		} catch (...) {
			Logf("[signaling] queue answer payload failed\n");
		}
	});

	pc->onLocalDescription([&](rtc::Description ld) {
		static std::atomic<int> ldCnt{0};
		const int c = ldCnt.fetch_add(1, std::memory_order_relaxed);
		if (c < 5) {
			Logf("[signaling] onLocalDescription type=%s ended=%d\n",
				ld.typeString().c_str(), (int)ld.ended());
		}
	});

	pc->onTrack([&](std::shared_ptr<rtc::Track> track) {
		if (!track) return;
		const std::string mid = track->mid();
		const std::string mediaType = track->description().type();
		Logf("[receiver] onTrack mid=%s mediaType=%s\n", mid.c_str(), mediaType.c_str());
		if (mediaType != "video") {
			Logf("[receiver] onTrack ignored (not video)\n");
			return;
		}
		if (g_videoTrackAttached.exchange(true, std::memory_order_acq_rel)) return;
		Logf("[receiver] onVideoTrack attached\n");

		g_videoTrackKeepAlive = track;

		auto depacketizer = std::make_shared<rtc::H264RtpDepacketizer>(
			rtc::H264RtpDepacketizer::Separator::LongStartSequence);
		try {
			track->setMediaHandler(depacketizer);
		} catch (const std::exception& e) {
			Logf("[receiver] setMediaHandler failed: %s\n", e.what());
			return;
		} catch (...) {
			Logf("[receiver] setMediaHandler failed: unknown\n");
			return;
		}

		track->onFrame([&decoder](rtc::binary data, rtc::FrameInfo /*info*/) {
			static std::atomic<uint64_t> frameCbCnt{0};
			static std::atomic<uint64_t> emptyCbCnt{0};

			const uint64_t cbn = frameCbCnt.fetch_add(1, std::memory_order_relaxed);
			const bool isEmpty = data.empty();

			if (isEmpty) {
				const uint64_t en = emptyCbCnt.fetch_add(1, std::memory_order_relaxed);
				if (en < 5) {
					Logf("[receiver] onFrame callback but data EMPTY (cbn=%llu)\n", (unsigned long long)cbn);
				}
				return;
			}

			if (cbn < 5) {
				Logf("[receiver] onFrame got rtc::binary size=%llu\n", (unsigned long long)data.size());
				const uint8_t* p = reinterpret_cast<const uint8_t*>(data.data());
				char hexbuf[80]{};
				const size_t dumpN = (data.size() < 16) ? data.size() : 16;
				for (size_t i = 0; i < dumpN && (i * 3 + 2) < sizeof(hexbuf); ++i) {
					std::snprintf(hexbuf + i * 3, 4, "%02X ", p[i]);
				}
				Logf("[receiver] onFrame prefix=%s\n", hexbuf);
			}

			const uint8_t* p = reinterpret_cast<const uint8_t*>(data.data());
			const size_t n = data.size();

			auto looksLikeAnnexB = [&]() -> bool {
				if (n >= 4 && p[0] == 0 && p[1] == 0 && p[2] == 0 && p[3] == 1) return true;
				if (n >= 3 && p[0] == 0 && p[1] == 0 && p[2] == 1) return true;
				return false;
			};

			const auto t_rx = std::chrono::steady_clock::now();
			try {
				// Bind agent SEI timestamps to the decode result.
				const uint64_t rxMs = SystemMsNow();
				g_pendingRxMs.store(rxMs, std::memory_order_relaxed);
				g_pendingRxMsValid.store(true, std::memory_order_release);

				uint64_t seiFrameId = 0;
				uint64_t seiCapMs = 0;
				uint64_t seiEncMs = 0;
				uint64_t seiSendMs = 0;
				const bool seiOk = looksLikeAnnexB() &&
					ExtractRpcLatencySeiFieldsFromAnnexB(p, n, seiFrameId, seiCapMs, seiEncMs, seiSendMs);
				if (seiOk) {
					g_pendingFrameId.store(seiFrameId, std::memory_order_relaxed);
					g_pendingCapMs.store(seiCapMs, std::memory_order_relaxed);
					g_pendingEncMs.store(seiEncMs, std::memory_order_relaxed);
					g_pendingSendMs.store(seiSendMs, std::memory_order_relaxed);
					g_pendingSeiValid.store(true, std::memory_order_release);
				} else {
					g_pendingSeiValid.store(false, std::memory_order_release);
				}

				bool decoded = decoder.decodeAnnexB(p, n);
				if (!decoded) {
					// Avoid leaking pending timestamps into a later successful decode.
					g_pendingSeiValid.store(false, std::memory_order_release);
					g_pendingRxMsValid.store(false, std::memory_order_release);
				}
				if (!decoded && !looksLikeAnnexB()) {
					std::vector<uint8_t> annexb;
					annexb.reserve(n + (n / 4) * 4);
					size_t off = 0;
					bool ok = true;
					while (off + 4 <= n) {
						const uint32_t len =
							(uint32_t(p[off + 0]) << 24) |
							(uint32_t(p[off + 1]) << 16) |
							(uint32_t(p[off + 2]) << 8) |
							(uint32_t(p[off + 3]) << 0);
						off += 4;
						if (len == 0 || off + size_t(len) > n) { ok = false; break; }
						annexb.insert(annexb.end(), { 0x00, 0x00, 0x00, 0x01 });
						annexb.insert(annexb.end(), p + off, p + off + len);
						off += size_t(len);
					}
					if (ok && !annexb.empty()) {
						uint64_t seiFrameId2 = 0;
						uint64_t seiCapMs2 = 0;
						uint64_t seiEncMs2 = 0;
						uint64_t seiSendMs2 = 0;
						const bool seiOk2 = ExtractRpcLatencySeiFieldsFromAnnexB(annexb.data(), annexb.size(), seiFrameId2, seiCapMs2, seiEncMs2, seiSendMs2);
						if (seiOk2) {
							g_pendingFrameId.store(seiFrameId2, std::memory_order_relaxed);
							g_pendingCapMs.store(seiCapMs2, std::memory_order_relaxed);
							g_pendingEncMs.store(seiEncMs2, std::memory_order_relaxed);
							g_pendingSendMs.store(seiSendMs2, std::memory_order_relaxed);
							g_pendingSeiValid.store(true, std::memory_order_release);
						} else {
							g_pendingSeiValid.store(false, std::memory_order_release);
						}

							// keep rxMs captured at onFrame callback entry for the final successful decode
							g_pendingRxMsValid.store(true, std::memory_order_release);
						decoded = decoder.decodeAnnexB(annexb.data(), annexb.size());
						if (!decoded) {
							g_pendingSeiValid.store(false, std::memory_order_release);
							g_pendingRxMsValid.store(false, std::memory_order_release);
						}
						if (decoded && cbn < 5) {
							Logf("[receiver] decodeAnnexB failed; length->AnnexB decode ok\n");
						}
					}
				}

				const auto t_after_decode = std::chrono::steady_clock::now();
				if (decoded) {
					static std::chrono::steady_clock::time_point s_last_decode_log{};
					if (t_after_decode - s_last_decode_log >= std::chrono::seconds(1)) {
						s_last_decode_log = t_after_decode;
						const int decode_us = static_cast<int>(
							std::chrono::duration_cast<std::chrono::microseconds>(t_after_decode - t_rx).count());
						Logf("[latency][recv_rx] onFrame_to_decode_done_us=%d bytes=%llu sysMs=%llu\n",
							decode_us,
							(unsigned long long)n,
							(unsigned long long)SystemMsNow());
					}
				}
			} catch (const std::exception& e) {
				if (cbn < 5) Logf("[receiver] decode exception: %s\n", e.what());
			} catch (...) {
				if (cbn < 5) Logf("[receiver] decode exception: unknown\n");
			}
		});
	});

	SetupDataChannelCallbacks(pc);
	return pc;
}

void RdrEnsureWinsockOnce() {
	EnsureWinsockOnce();
}

void RdrRunSignalingAndWebRtc(
	const std::string& host,
	int port,
	const std::string& clientId,
	const std::string& exePath,
	RdrVideoDecoderMode decoderMode
) {
	EnsureWinsockOnce();

	auto ws = std::make_shared<rtc::WebSocket>();
	std::atomic<bool> offerHandled{false};
	std::atomic<bool> wsOpened{false};
	std::atomic<bool> requestSent{false};

	const std::string exePathLocal = exePath;

	const std::string requestPayload = [exePathLocal] {
		json req = { {"id", "server"}, {"type", "request"} };
		if (!exePathLocal.empty()) req["exePath"] = exePathLocal;
		return req.dump();
	}();

	ws->onOpen([ws, exePathLocal, &wsOpened] {
		Logf("[signaling] ws onOpen fired\n");
		wsOpened.store(true, std::memory_order_relaxed);
	});

	ws->onError([](const std::string& err) {
		Logf("[signaling] WebSocket error: %s\n", err.c_str());
	});
	ws->onClosed([] {
		Logf("[signaling] WebSocket closed\n");
	});

	std::mutex pcMtx;
	std::shared_ptr<rtc::PeerConnection> pc;
	std::shared_ptr<IH264ToBGRADecoder> decoder;

	ws->onMessage([&](const std::variant<rtc::binary, std::string>& data) {
		if (std::holds_alternative<std::string>(data)) {
			const std::string& str = std::get<std::string>(data);
			json msg;
			try { msg = json::parse(str); }
			catch (...) {
				static std::atomic<int> parseFailCnt{0};
				int c = parseFailCnt.fetch_add(1, std::memory_order_relaxed);
				if (c < 5) Logf("[signaling] onMessage JSON parse failed\n");
				return;
			}

			const std::string type = msg.value("type", "");
			static std::atomic<int> recvMsgCnt{0};
			int rc = recvMsgCnt.fetch_add(1, std::memory_order_relaxed);
			if (rc < 20) Logf("[signaling] onMessage type=%s\n", type.c_str());
			if (type == "offer") {
				const std::string sdp = msg.value("sdp", "");

				std::lock_guard<std::mutex> lk(pcMtx);
				if (!pc) {
					if (!decoder) {
						auto publish = [](int w, int h, uint64_t decodedIndex, std::vector<uint8_t>&& bgra) {
							const auto queued = std::chrono::steady_clock::now();
							const bool haveSei = g_pendingSeiValid.exchange(false, std::memory_order_acq_rel);
							const uint64_t frameId = haveSei ? g_pendingFrameId.load(std::memory_order_relaxed) : 0;
							const uint64_t capMs = haveSei ? g_pendingCapMs.load(std::memory_order_relaxed) : 0;
							const uint64_t encMs = haveSei ? g_pendingEncMs.load(std::memory_order_relaxed) : 0;
							const uint64_t sendMs = haveSei ? g_pendingSendMs.load(std::memory_order_relaxed) : 0;

							const bool haveRx = g_pendingRxMsValid.exchange(false, std::memory_order_acq_rel);
							const uint64_t rxMs = haveRx ? g_pendingRxMs.load(std::memory_order_relaxed) : 0;
							const uint64_t decDoneMs = SystemMsNow();
							{
								std::lock_guard<std::mutex> lk2(g_frameMtx);
								g_sharedFrame.w = w;
								g_sharedFrame.h = h;
								g_sharedFrame.decodedIndex = decodedIndex;
								g_sharedFrame.bgra = std::move(bgra);
								g_sharedFrame.frameId = frameId;
								g_sharedFrame.capMs = capMs;
								g_sharedFrame.encMs = encMs;
								g_sharedFrame.sendMs = sendMs;
								g_sharedFrame.hasAgentTimes = haveSei;

								g_sharedFrame.rxMs = rxMs;
								g_sharedFrame.decDoneMs = decDoneMs;
								g_sharedFrame.hasRxDecTimes = haveRx;
								g_sharedFrame.decodeQueuedSteady = queued;
								g_sharedFrame.hasDecodeQueuedSteady = true;
								g_sharedFrame.ready = true;
								g_videoW.store(w, std::memory_order_relaxed);
								g_videoH.store(h, std::memory_order_relaxed);
							}
							RdrNotifyNewVideoFrame();
						};
						try {
							if (decoderMode == RdrVideoDecoderMode::Sw) {
								decoder = std::make_shared<H264AnnexBDecoderToBGRA>(publish);
								Logf("[decoder] using software (FFmpeg CPU)\n");
							} 
							else if (decoderMode == RdrVideoDecoderMode::Hw) {
								decoder = std::make_shared<H264D3D11VADecoderToBGRA>(g_d3d.device.Get(), std::move(publish));
								Logf("[decoder] using D3D11VA (FFmpeg hwaccel)\n");
							} else {
								try {
									decoder = std::make_shared<H264D3D11VADecoderToBGRA>(g_d3d.device.Get(), publish);
									Logf("[decoder] auto: D3D11VA ok\n");
								} catch (const std::exception& e) {
									Logf("[decoder] auto: D3D11VA failed (%s), software fallback\n", e.what());
									decoder = std::make_shared<H264AnnexBDecoderToBGRA>(publish);
								}
							}
						} catch (const std::exception& e) {
							Logf("[decoder] FFmpeg init failed: %s\n", e.what());
							return;
						}
					}
					pc = SetupPeerConnectionForAnswerer(ws, offerHandled, *decoder);

					rtc::Description offer(sdp, type);
					if (pc) {
						pc->setRemoteDescription(offer);
						pc->setLocalDescription(rtc::Description::Type::Answer);
						pc->gatherLocalCandidates();
					}
				}
			}
		}
	});

	const std::string url = "ws://" + host + ":" + std::to_string(port) + "/" + clientId;
	std::printf("[signaling] ws url: %s\n", url.c_str());
	try {
		ws->open(url);
	} catch (const std::exception& e) {
		Logf("[signaling] ws->open failed: %s\n", e.what());
		throw;
	} catch (...) {
		Logf("[signaling] ws->open failed: unknown exception\n");
		throw;
	}

	{
		std::lock_guard<std::mutex> lk(g_wsMtx);
		g_wsForRequest = ws;
	}

	Logf("[signaling] ws->open returned, isOpen=%d isClosed=%d\n",
		ws ? (int)ws->isOpen() : 0,
		ws ? (int)ws->isClosed() : 1);

	static std::atomic<int> loopLogCnt{0};
	while (!g_exitRequested.load(std::memory_order_relaxed) && ws && !ws->isClosed()) {
		const int lc = loopLogCnt.fetch_add(1, std::memory_order_relaxed);
		if (lc < 5) {
			Logf("[signaling] loop iter=%d wsOpened=%d requestSent=%d isOpen=%d\n",
				lc,
				(int)wsOpened.load(std::memory_order_relaxed),
				(int)requestSent.load(std::memory_order_relaxed),
				(int)ws->isOpen());
		}
		if (wsOpened.load(std::memory_order_relaxed) && !requestSent.load(std::memory_order_relaxed)) {
			try {
				Logf("[signaling] sending request from signaling thread\n");
				ws->send(requestPayload);
				requestSent.store(true, std::memory_order_relaxed);
				Logf("[signaling] request sent ok\n");
			} catch (const std::exception& e) {
				Logf("[signaling] ws->send request failed: %s\n", e.what());
				requestSent.store(true, std::memory_order_relaxed);
			} catch (...) {
				Logf("[signaling] ws->send request failed: unknown\n");
				requestSent.store(true, std::memory_order_relaxed);
			}
		}

		DrainWsSendPending();
		std::this_thread::sleep_for(std::chrono::milliseconds(200));
	}
}
