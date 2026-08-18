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

option("tsan")
    set_default(false)
    set_showmenu(true)
    set_description("Build with ThreadSanitizer (xmake f --tsan=y); " ..
                    "mutually exclusive with --asan")
option_end()

if has_config("asan") then
    set_policy("build.sanitizer.address", true)
    set_policy("build.sanitizer.leak", true)
elseif has_config("tsan") then
    -- xmake has no thread-sanitizer policy; wire the flags directly.
    add_cxflags("-fsanitize=thread", {force = true})
    add_ldflags("-fsanitize=thread", {force = true})
end

add_requires("workflow v1.0.1")
add_requires("openssl")
add_requires("gtest", {configs = {main = true}})

includes("server", "client", "tests")
