#pragma once
  
#include <variant>     // for variant
#include <memory>      // for shared_ptr

namespace spotify {
  
  enum CommandType {
    STOP,
    PLAY,
    PAUSE,
    DISC,
    DEPLETED,
    FLUSH,
    PLAYBACK_START,
    PLAYBACK,
    SKIP_NEXT,
    SKIP_PREV,
    SEEK,
    SET_SHUFFLE,
    SET_REPEAT,
    VOLUME,
    TRACK_INFO,
  }; 
  
  typedef std::variant<std::shared_ptr<spotify::QueuedTrack>, int32_t, bool>
      CommandData;

  struct Command {
    CommandType commandType;
    CommandData data;
  };
}