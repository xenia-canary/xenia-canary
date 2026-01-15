group("third_party")
project("asio")
  uuid("a5f06be0-9c7d-4e8f-b2a1-3d4c5e6f7890")
  kind("Utility")
  language("C++")

  includedirs({
    "asio/include",
  })

  defines({
    "ASIO_STANDALONE",
    "ASIO_NO_DEPRECATED",
  })

  filter("platforms:Windows")
    defines({
      "_WIN32_WINNT=0x0A00",  -- Windows 10+
    })
  filter({})

  files({
    "asio/asio/include/asio.hpp",
  })

  warnings("Off")
