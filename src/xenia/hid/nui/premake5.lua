-- Kinectix NUI build configuration.
-- Included from src/xenia/hid/premake5.lua via `include("nui")`.
--
-- The recorded backend is always built (no external dependencies).
-- libfreenect / libfreenect2 / mediapipe backends are gated by build flags
-- so a default `xb build` produces a working binary on a machine with no
-- Kinect tooling installed.

project("xenia-hid-nui")
  uuid("8a7b4e80-1c18-46a0-9c4e-9d2c6c4f7b84")
  kind("StaticLib")
  language("C++")
  links({
    "fmt",
    "flatbuffers",  -- for xnuirec_generated.h
    "xenia-base",
  })
  defines({
  })
  includedirs({
    project_root.."/src",
    project_root.."/third_party/flatbuffers/include",
  })
  local_platform_files()
  files({
    -- Core
    "nui_backend.h",
    "nui_constants.h",
    "nui_manager.h",
    "nui_manager.cc",
    "nui_null_backend.h",

    -- Recorded backend (always built)
    "recorded/recorded_backend.h",
    "recorded/recorded_backend.cc",
    "recorded/xnuirec_reader.h",
    "recorded/xnuirec_reader.cc",
    "recorded/xnuirec_writer.h",
    "recorded/xnuirec_writer.cc",
  })

  -- Generate xnuirec_generated.h from xnuirec.fbs at build time.
  -- The generated file lives in obj/<config>/generated/.
  filter("files:**/xnuirec.fbs")
    buildmessage("flatc xnuirec.fbs")
    buildcommands({
      "flatc --cpp -o " .. path.getabsolute("recorded/generated") ..
      " %{file.relpath}",
    })
    buildoutputs({
      path.getabsolute("recorded/generated/xnuirec_generated.h"),
    })
  filter({})

  -- Optional libfreenect (Kinect v1) backend.
  filter("options:KINECTIX_NUI_FREENECT")
    defines({"XE_KINECTIX_NUI_FREENECT=1"})
    files({
      "freenect/freenect_backend.h",
      "freenect/freenect_backend.cc",
      "freenect/skeleton_tracker.h",
      "freenect/skeleton_tracker.cc",
    })
    links({"freenect"})
  filter({})

  -- Optional libfreenect2 (Kinect v2) backend.
  filter("options:KINECTIX_NUI_FREENECT2")
    defines({"XE_KINECTIX_NUI_FREENECT2=1"})
    files({
      "freenect/freenect2_backend.h",
      "freenect/freenect2_backend.cc",
    })
    links({"freenect2"})
  filter({})

  -- Optional MediaPipe backend (webcam + ML pose).
  filter("options:KINECTIX_NUI_MEDIAPIPE")
    defines({"XE_KINECTIX_NUI_MEDIAPIPE=1"})
    files({
      "mediapipe/mediapipe_backend.h",
      "mediapipe/mediapipe_backend.cc",
      "mediapipe/pose_to_skeleton.h",
      "mediapipe/pose_to_skeleton.cc",
    })
    links({"mediapipe", "opencv_core", "opencv_videoio"})
  filter({})

  -- Tests live in their own project so the main lib stays slim.

newoption({
  trigger     = "KINECTIX_NUI_FREENECT",
  description = "Build the libfreenect (Kinect v1) NUI backend.",
})
newoption({
  trigger     = "KINECTIX_NUI_FREENECT2",
  description = "Build the libfreenect2 (Kinect v2) NUI backend.",
})
newoption({
  trigger     = "KINECTIX_NUI_MEDIAPIPE",
  description = "Build the MediaPipe (webcam pose) NUI backend.",
})
