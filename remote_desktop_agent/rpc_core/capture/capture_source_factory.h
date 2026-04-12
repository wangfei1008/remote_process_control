#pragma once

#include "capture/capture_kind_resolver.h"
#include "capture/i_capture_source.h"

#include <memory>

std::unique_ptr<ICaptureSource> create_capture_source(ProcessCaptureKind kind);
