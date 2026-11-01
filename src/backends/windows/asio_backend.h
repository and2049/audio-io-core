#ifndef AUDIO_ENGINE_WINDOWS_ASIO_BACKEND_H
#define AUDIO_ENGINE_WINDOWS_ASIO_BACKEND_H

#include <windows.h>

#include <objbase.h>

#include <iasiodrv.h>
#include <memory>

#include "backend.h"

namespace ae {
using AsioFactory = HRESULT (*)(const CLSID &, IASIO **);
std::unique_ptr<Backend> make_asio_backend(AsioFactory factory = nullptr,
                                           std::vector<Device> devices = {});
}

#endif
