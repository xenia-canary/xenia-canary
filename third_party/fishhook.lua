group("third_party")
project("fishhook")
  uuid("b8f96d88-f149-48fd-870e-41f578a0c019")
  kind("StaticLib")
  language("C")

  includedirs({
    ".",
  })
  files({
    "fishhook.c",
    "fishhook.h",
  })
