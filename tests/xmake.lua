set_group("test")

add_packages("gtest")
add_links("gtest_main")

target("unit-test")
    set_kind("binary")
    add_files("unit/**.cc")
    add_deps("ferry-server-core", "ferry-client-core")
target_end()

target("integration-test")
    set_kind("binary")
    add_files("integration/**.cc")
    add_deps("ferry-server-core", "ferry-client-core")
target_end()
