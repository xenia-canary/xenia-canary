project_root = "../../../.."
include(project_root.."/tools/build")

test_suite("xenia-cpu-tests", project_root, ".", {
  links = {
    "capstone",
    "fmt",
    "imgui",
    "xenia-base",
    "xenia-core",
    "xenia-cpu",
    "xenia-gpu",

    -- TODO(benvanik): cut these dependencies?
    "xenia-kernel",
    "xenia-ui", -- needed by xenia-base
    "xenia-patcher",
    "libavcodec",
    "libavutil",
  },
  filtered_links = {
    {
      filter = 'architecture:x86_64',
      links = {
        "xenia-cpu-backend-x64",
      },
    },
    {
      filter = 'architecture:ARM64',
      links = {
        "xenia-cpu-backend-a64",
      },
    },
    {
      filter = 'platforms:Mac-*',
      links = {
        "Cocoa.framework",
      },
    },
    {
      filter = 'platforms:not Mac-*',
      links = {
        "libavformat",
      },
    },
  },
})
