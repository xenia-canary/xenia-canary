SINGLE_LIBRARY_PLATFORM_PATTERNS = {
    "Android-*",
};

SINGLE_LIBRARY_FILTER =
    "platforms:" .. table.concat(SINGLE_LIBRARY_PLATFORM_PATTERNS, " or ");
NOT_SINGLE_LIBRARY_FILTER = table.translate(
    SINGLE_LIBRARY_PLATFORM_PATTERNS,
    function(pattern)
      return "platforms:not " .. pattern;
    end);

function single_library_windowed_app_kind()
  filter(SINGLE_LIBRARY_FILTER);
    kind("StaticLib");
    if type(wholelib) == "function" then
      wholelib("On");
    end
  filter(NOT_SINGLE_LIBRARY_FILTER);
    kind("WindowedApp");
  filter({});
end
