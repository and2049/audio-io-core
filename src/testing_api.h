#ifndef AUDIO_ENGINE_SRC_TESTING_API_H
#define AUDIO_ENGINE_SRC_TESTING_API_H

#include <memory>

#include "audio_engine/engine.h"
#include "backend.h"

namespace ae::detail {

ae_result create_context_with_backend(std::unique_ptr<Backend> backend, ae_context **out_context);

}

#endif
