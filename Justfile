# Justfile for Cosmos
set shell := ["bash", "-uc"]

# Each devShell sets its own BUILD_DIR so toolchains never share a cmake cache.
build_dir := env_var_or_default("BUILD_DIR", "build")

default:
    @just --list

# Build production binaries
build:
    cmake -B {{build_dir}} -DCMAKE_BUILD_TYPE=Release
    cmake --build {{build_dir}} --parallel --target kv_store_prod replicated_kv_prod

# Build simulation binaries only
sim:
    cmake -B {{build_dir}} -DCOSMOS_BUILD_TESTS=OFF
    cmake --build {{build_dir}} --parallel --target kv_store_sim replicated_kv_sim

# Build and run unit tests
test:
    cmake -B {{build_dir}} -DCOSMOS_BUILD_TESTS=ON
    cmake --build {{build_dir}} --parallel
    ctest --test-dir {{build_dir}} --output-on-failure

# Build all targets (production binaries, simulation examples, and tests)
all:
    cmake -B {{build_dir}} -DCOSMOS_BUILD_TESTS=ON
    cmake --build {{build_dir}} --parallel

# Format all C/C++ source and header files
format:
    find include src tests examples -type f \( -name "*.cpp" -o -name "*.hpp" -o -name "*.c" -o -name "*.h" \) -exec clang-format -i {} +

# Check code formatting without modifying files
format-check:
    find include src tests examples -type f \( -name "*.cpp" -o -name "*.hpp" -o -name "*.c" -o -name "*.h" \) -exec clang-format --dry-run --Werror {} +

# Lint codebase (alias for format-check)
lint: format-check

# Remove this shell's build directory
clean:
    rm -rf {{build_dir}}

alias b := build
alias s := sim
alias t := test
alias fmt := format
alias l := lint
alias c := clean
