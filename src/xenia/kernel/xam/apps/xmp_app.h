/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_XAM_APPS_XMP_APP_H_
#define XENIA_KERNEL_XAM_APPS_XMP_APP_H_

#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/xam/app_manager.h"

namespace xe {
namespace kernel {
namespace xam {
namespace apps {

// Only source of docs for a lot of these functions:
// https://github.com/oukiar/freestyledash/blob/master/Freestyle/Scenes/Media/Music/ScnMusic.cpp

struct XMP_SONGDESCRIPTOR {
  xe::be<uint32_t> file_path_ptr;
  xe::be<uint32_t> title_ptr;
  xe::be<uint32_t> artist_ptr;
  xe::be<uint32_t> album_ptr;
  xe::be<uint32_t> album_artist_ptr;
  xe::be<uint32_t> genre_ptr;
  xe::be<uint32_t> track_number;
  xe::be<uint32_t> duration;
  xe::be<uint32_t> song_format;
};
static_assert_size(XMP_SONGDESCRIPTOR, 36);

constexpr uint32_t kMaxXmpMetadataStringLength = 40;

struct XMP_SONGINFO {
  X_HANDLE handle;

  uint8_t unknown[0x23C];
  xe::be<char16_t> title[kMaxXmpMetadataStringLength];
  xe::be<char16_t> artist[kMaxXmpMetadataStringLength];
  xe::be<char16_t> album[kMaxXmpMetadataStringLength];
  xe::be<char16_t> album_artist[kMaxXmpMetadataStringLength];
  xe::be<char16_t> genre[kMaxXmpMetadataStringLength];
  xe::be<uint32_t> track_number;
  xe::be<uint32_t> duration;
  xe::be<uint32_t> song_format;
};
static_assert_size(XMP_SONGINFO, 988);

struct XMP_PLAY_TITLE_PLAYLIST {
  xe::be<uint32_t> xmp_client;
  xe::be<uint32_t> storage_ptr;
  xe::be<uint32_t> song_handle;
};
static_assert_size(XMP_PLAY_TITLE_PLAYLIST, 0xC);

struct XMP_STOP {
  xe::be<uint32_t> xmp_client;
  xe::be<uint32_t> unk;
};
static_assert_size(XMP_STOP, 0x8);

struct XMP_SET_PLAYBACK_BEHAVIOR {
  xe::be<uint32_t> xmp_client;
  xe::be<uint32_t> playback_mode;
  xe::be<uint32_t> repeat_mode;
  xe::be<uint32_t> flags;
};
static_assert_size(XMP_SET_PLAYBACK_BEHAVIOR, 0x10);

struct XMP_GET_STATUS {
  xe::be<uint32_t> xmp_client;
  xe::be<uint32_t> state_ptr;
};
static_assert_size(XMP_GET_STATUS, 0x8);

struct XMP_GET_VOLUME {
  xe::be<uint32_t> xmp_client;
  xe::be<uint32_t> volume_ptr;
};
static_assert_size(XMP_GET_VOLUME, 0x8);

struct XMP_SET_VOLUME {
  xe::be<uint32_t> xmp_client;
  xe::be<float> value;
};
static_assert_size(XMP_SET_VOLUME, 0x8);

struct XMP_CREATE_TITLE_PLAYLIST {
  xe::be<uint32_t> xmp_client;
  xe::be<uint32_t> storage_ptr;
  xe::be<uint32_t> storage_size;
  xe::be<uint32_t> songs_ptr;
  xe::be<uint32_t> song_count;
  xe::be<uint32_t> playlist_name_ptr;
  xe::be<uint32_t> flags;
  xe::be<uint32_t> song_handles_ptr;
  xe::be<uint32_t> playlist_handle_ptr;
};
static_assert_size(XMP_CREATE_TITLE_PLAYLIST, 0x24);

struct XMP_GET_CURRENT_SONG {
  xe::be<uint32_t> xmp_client;
  xe::be<uint32_t> unk_ptr;
  xe::be<uint32_t> info_ptr;
};
static_assert_size(XMP_GET_CURRENT_SONG, 0xC);

struct XMP_DELETE_TITLE_PLAYLIST {
  xe::be<uint32_t> xmp_client;
  xe::be<uint32_t> storage_ptr;
};
static_assert_size(XMP_DELETE_TITLE_PLAYLIST, 0x8);

struct XMP_SET_PLAYBACK_CONTROLLER {
  xe::be<uint32_t> xmp_client;
  xe::be<uint32_t> controller;
  xe::be<uint32_t> playback_client;
};
static_assert_size(XMP_SET_PLAYBACK_CONTROLLER, 0xC);

struct XMP_GET_PLAYBACK_CONTROLLER {
  xe::be<uint32_t> xmp_client;
  xe::be<uint32_t> controller_ptr;
  xe::be<uint32_t> locked_ptr;
};
static_assert_size(XMP_GET_PLAYBACK_CONTROLLER, 0xC);

struct XMP_CREATE_USER_PLAYLIST_ENUMERATOR {
  xe::be<uint32_t> xmp_client;
  xe::be<uint32_t> flags;
  xe::be<uint32_t> object_ptr;
};
static_assert_size(XMP_CREATE_USER_PLAYLIST_ENUMERATOR, 0xC);

struct XMP_GET_PLAYBACK_BEHAVIOR {
  xe::be<uint32_t> xmp_client;
  xe::be<uint32_t> playback_mode_ptr;
  xe::be<uint32_t> repeat_mode_ptr;
  xe::be<uint32_t> playback_flags_ptr;
};
static_assert_size(XMP_GET_PLAYBACK_BEHAVIOR, 0x10);

struct XMP_GET_MEDIA_SOURCES {
  xe::be<uint32_t> xmp_client;
  xe::be<uint32_t> unk1;
  xe::be<uint32_t> unk1_ptr;
  xe::be<uint32_t> unk2;
  xe::be<uint32_t> unk2_ptr;
};
static_assert_size(XMP_GET_MEDIA_SOURCES, 0x14);

struct XMP_GET_TITLE_PLAYLIST_BUFFER_SIZE {
  xe::be<uint32_t> xmp_client;
  xe::be<uint32_t> song_count;
  xe::be<uint32_t> size_ptr;
};
static_assert_size(XMP_GET_TITLE_PLAYLIST_BUFFER_SIZE, 0xC);

struct XMP_DASH_INIT {
  xe::be<uint32_t> xmp_client;
  xe::be<uint32_t> buffer_ptr;     // used by XamEnumerate
  xe::be<uint32_t> buffer_length;  // used by XamEnumerate
  xe::be<uint32_t> unk1;
  xe::be<uint32_t> unk2;
  xe::be<uint32_t> storage_ptr;
};
static_assert_size(XMP_DASH_INIT, 0x18);

struct XMP_CAPTURE_OUTPUT {
  xe::be<uint32_t> xmp_client;
  xe::be<uint32_t> callback;
  xe::be<uint32_t> context;
  xe::be<uint32_t> title_render;
};
static_assert_size(XMP_CAPTURE_OUTPUT, 0x10);

struct XMP_SET_MEDIA_SOURCE_WORKSPACE {
  xe::be<uint32_t> xmp_client;
  xe::be<uint32_t> unk1;
  xe::be<uint32_t> storage_ptr;
  xe::be<uint32_t> unk2;
};
static_assert_size(XMP_SET_MEDIA_SOURCE_WORKSPACE, 0x10);

struct XMP_GET_DASH_INIT_STATE {
  xe::be<uint32_t> xmp_client;
  xe::be<uint32_t> dash_init_state_ptr;
};
static_assert_size(XMP_GET_DASH_INIT_STATE, 0x8);

class XmpApp : public App {
 public:
  enum class State : uint32_t {
    kIdle = 0,
    kPlaying = 1,
    kPaused = 2,
  };
  enum class PlaybackClient : uint32_t {
    kSystem = 0,
    kTitle = 1,
  };
  enum class PlaybackMode : uint32_t {
    kInOrder = 0,
    kShuffle = 1,
  };
  enum class RepeatMode : uint32_t {
    kPlaylist = 0,
    kNoRepeat = 1,
  };
  enum class PlaybackFlags : uint32_t {
    kDefault = 0,
    kAutoPause = 1,
  };
  struct Song {
    enum class Format : uint32_t {
      kWma = 0,
      kMp3 = 1,
    };

    uint32_t handle;
    std::u16string file_path;
    std::u16string name;
    std::u16string artist;
    std::u16string album;
    std::u16string album_artist;
    std::u16string genre;
    uint32_t track_number;
    uint32_t duration_ms;
    Format format;
  };
  struct Playlist {
    uint32_t handle;
    std::u16string name;
    uint32_t flags;
    std::vector<std::unique_ptr<Song>> songs;
  };

  explicit XmpApp(KernelState* kernel_state);

  X_HRESULT XMPGetStatus(uint32_t status_ptr);

  X_HRESULT XMPCreateTitlePlaylist(uint32_t songs_ptr, uint32_t song_count,
                                   uint32_t playlist_name_ptr,
                                   const std::u16string& playlist_name,
                                   uint32_t flags, uint32_t out_song_handles,
                                   uint32_t out_playlist_handle);
  X_HRESULT XMPDeleteTitlePlaylist(uint32_t playlist_handle);
  X_HRESULT XMPPlayTitlePlaylist(uint32_t playlist_handle,
                                 uint32_t song_handle);
  X_HRESULT XMPContinue();
  X_HRESULT XMPStop(uint32_t unk);
  X_HRESULT XMPPause();
  X_HRESULT XMPNext();
  X_HRESULT XMPPrevious();
  X_HRESULT XMPGetTitlePlaylistBufferSize(uint32_t xmp_client,
                                          uint32_t song_count,
                                          uint32_t storage_ptr);

  X_HRESULT DispatchMessageSync(uint32_t message, uint32_t buffer_ptr,
                                uint32_t buffer_length) override;

 private:
  xe::global_critical_region global_critical_region_;

  // TODO: Remove it and replace with guest handles!
  uint32_t next_playlist_handle_ = 0;
  uint32_t next_song_handle_ = 0;
};

}  // namespace apps
}  // namespace xam
}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_XAM_APPS_XMP_APP_H_
