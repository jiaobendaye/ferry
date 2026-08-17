-- Core library: everything except main.cc. Shared by the client binary
-- and the test binaries.
target("ferry-client-core")
    set_kind("static")
    add_files("*.cc|main.cc")
    add_includedirs(".", {public = true})
    add_packages("workflow", "openssl", {public = true})
    add_syslinks("pthread", "dl", {public = true})

target("ferry-client")
    set_kind("binary")
    add_files("main.cc")
    add_deps("ferry-client-core")
