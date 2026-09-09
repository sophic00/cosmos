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

# Run the 3-step cache demo (prod, tolerated fault, unhandled fault)
demo:
    cmake -B {{build_dir}} -DCOSMOS_BUILD_TESTS=OFF
    cmake --build {{build_dir}} --parallel --target cache_demo_prod cache_demo_sim
    @echo "--- Step 1: normal run, no libcosmos (faults impossible) ---"
    ./{{build_dir}}/examples/single_node/cache_demo_prod
    @echo "--- Step 2: sim seed 1006 (PUT key14 OOM, tolerated) ---"
    ./{{build_dir}}/examples/single_node/cache_demo_sim --seed 1006
    @echo "--- Step 3: sim seed 991 (cache_create OOM, not handled; exit 1 is the finding) ---"
    ./{{build_dir}}/examples/single_node/cache_demo_sim --seed 991 || true
    @echo "--- Demo done: same app, three universes ---"

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
alias d := demo
alias fmt := format
alias l := lint
alias c := clean
