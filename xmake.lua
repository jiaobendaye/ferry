set_project("ferry")
set_version("0.1.0")

add_rules("mode.debug", "mode.release")

set_languages("c++17")
set_warnings("all")

option("asan")
    set_default(false)
    set_showmenu(true)
    set_description("Build with AddressSanitizer (xmake f --asan=y)")
option_end()

if has_config("asan") then
    set_policy("build.sanitizer.address", true)
    set_policy("build.sanitizer.leak", true)
end

add_requires("workflow v1.0.1")
add_requires("openssl")
add_requires("gtest", {configs = {main = true}})

includes("server", "client", "tests")
