# The installer is copied outside the application before it runs. Keep it free of
# Qt and application library dependencies so it can replace those files.
add_executable(kyty_updater main.cpp UpdateInstaller.cpp)

target_compile_options(kyty_updater PRIVATE -fexceptions)
