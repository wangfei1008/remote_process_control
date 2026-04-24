#pragma once

#include "common/remote_video_contract.h"
#include "capture/i_capture_source.h"

#include <memory>

std::unique_ptr<ICaptureSource> create_capture_source();
