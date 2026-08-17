-- Core library: everything except main.cc. Shared by the server binary
-- and both test binaries.
target("ferry-server-core")
    set_kind("static")
    add_files("*.cc|main.cc")
    add_includedirs(".", {public = true})
    add_packages("workflow", {public = true})
    add_syslinks("pthread", "dl", {public = true})

target("ferry-server")
    set_kind("binary")
    add_files("main.cc")
    add_deps("ferry-server-core")
