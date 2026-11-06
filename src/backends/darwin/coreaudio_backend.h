#ifndef AUDIO_ENGINE_DARWIN_COREAUDIO_BACKEND_H
#define AUDIO_ENGINE_DARWIN_COREAUDIO_BACKEND_H

#include <CoreAudio/CoreAudio.h>

#include <memory>

#include "backend.h"

namespace ae {
enum class CaptureAccess { Undetermined, Denied, Granted };

class CoreAudioHal {
public:
  virtual ~CoreAudioHal() = default;
  virtual OSStatus size(AudioObjectID object, const AudioObjectPropertyAddress &address,
                        UInt32 &bytes) = 0;
  virtual OSStatus get(AudioObjectID object, const AudioObjectPropertyAddress &address,
                       UInt32 &bytes, void *data) = 0;
  virtual OSStatus set(AudioObjectID object, const AudioObjectPropertyAddress &address,
                       UInt32 bytes, const void *data) = 0;
  virtual OSStatus listen(AudioObjectID object, const AudioObjectPropertyAddress &address,
                          AudioObjectPropertyListenerProc listener, void *client) = 0;
  virtual OSStatus unlisten(AudioObjectID object, const AudioObjectPropertyAddress &address,
                            AudioObjectPropertyListenerProc listener, void *client) = 0;
  virtual OSStatus create_io(AudioObjectID device, AudioDeviceIOProc proc, void *client,
                             AudioDeviceIOProcID &id) = 0;
  virtual OSStatus destroy_io(AudioObjectID device, AudioDeviceIOProcID id) = 0;
  virtual OSStatus start(AudioObjectID device, AudioDeviceIOProcID id) = 0;
  virtual OSStatus stop(AudioObjectID device, AudioDeviceIOProcID id) = 0;
  virtual OSStatus create_aggregate(CFDictionaryRef description, AudioObjectID &device) = 0;
  virtual OSStatus destroy_aggregate(AudioObjectID device) = 0;
  virtual CaptureAccess capture_access(bool request) = 0;
};

std::unique_ptr<Backend> make_coreaudio_backend(CoreAudioHal *hal = nullptr);
}

#endif
