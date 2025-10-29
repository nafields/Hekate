# USB Host Support - Testing Framework

## Overview
This document outlines a comprehensive testing strategy for the USB host support implementation, including GitHub Actions workflows for CI/CD.

## Testing Layers

### Layer 1: Static Analysis (Automated)
- Code compilation checks
- Static code analysis
- Code style validation
- Documentation checks

### Layer 2: Unit Tests (Future)
- Individual function testing
- Mock USB device responses
- Error condition testing

### Layer 3: Integration Tests (Hardware Required)
- Real device enumeration
- Data transfer verification
- Performance benchmarking

### Layer 4: Compatibility Tests (Hardware Required)
- Multiple device types
- Various manufacturers
- Edge cases and stress tests

## GitHub Actions Workflows

### Workflow 1: Compilation Check

**File**: `.github/workflows/build-check.yml`

This ensures the code compiles without the USB host changes breaking the build:

```yaml
name: Build Check

on:
  push:
    branches: [ main, develop, 'copilot/**' ]
  pull_request:
    branches: [ main, develop ]

jobs:
  build-check:
    runs-on: ubuntu-latest
    
    steps:
    - name: Checkout repository
      uses: actions/checkout@v4
      with:
        submodules: recursive
        
    - name: Cache devkitARM
      uses: actions/cache@v3
      id: cache-devkitarm
      with:
        path: /opt/devkitpro
        key: ${{ runner.os }}-devkitarm-r64
        
    - name: Install devkitARM
      if: steps.cache-devkitarm.outputs.cache-hit != 'true'
      run: |
        wget https://github.com/devkitPro/pacman/releases/latest/download/devkitpro-pacman.amd64.deb
        sudo dpkg -i devkitpro-pacman.amd64.deb
        sudo dkp-pacman -Sy
        sudo dkp-pacman -S --noconfirm devkitARM
        
    - name: Set environment variables
      run: |
        echo "DEVKITPRO=/opt/devkitpro" >> $GITHUB_ENV
        echo "DEVKITARM=/opt/devkitpro/devkitARM" >> $GITHUB_ENV
        echo "/opt/devkitpro/devkitARM/bin" >> $GITHUB_PATH
        
    - name: Build bootloader
      run: |
        make -j$(nproc)
        
    - name: Build Nyx
      run: |
        cd nyx
        make -j$(nproc)
        
    - name: Upload artifacts
      if: success()
      uses: actions/upload-artifact@v3
      with:
        name: hekate-binaries
        path: |
          output/*.bin
          nyx/output/*.bin
        retention-days: 7
        
    - name: Check file sizes
      run: |
        echo "Checking binary sizes..."
        ls -lh output/*.bin
        ls -lh nyx/output/*.bin
```

### Workflow 2: Static Analysis

**File**: `.github/workflows/static-analysis.yml`

```yaml
name: Static Analysis

on:
  push:
    branches: [ main, develop, 'copilot/**' ]
  pull_request:
    branches: [ main, develop ]

jobs:
  cppcheck:
    runs-on: ubuntu-latest
    
    steps:
    - name: Checkout repository
      uses: actions/checkout@v4
      
    - name: Install cppcheck
      run: sudo apt-get install -y cppcheck
      
    - name: Run cppcheck on USB files
      run: |
        cppcheck --enable=warning,style,performance,portability \
                 --error-exitcode=1 \
                 --inline-suppr \
                 -I bdk \
                 --suppress=missingIncludeSystem \
                 bdk/usb/xhci.c \
                 bdk/usb/usb_msc_host.c \
                 nyx/nyx_gui/emummc_storage_usb.c
                 
  clang-tidy:
    runs-on: ubuntu-latest
    
    steps:
    - name: Checkout repository
      uses: actions/checkout@v4
      
    - name: Install clang-tidy
      run: sudo apt-get install -y clang-tidy
      
    - name: Run clang-tidy
      run: |
        clang-tidy bdk/usb/*.c nyx/nyx_gui/emummc_storage_usb.c \
          -checks='readability-*,modernize-*,performance-*' \
          -- -I bdk -I nyx/nyx_gui
          
  documentation-check:
    runs-on: ubuntu-latest
    
    steps:
    - name: Checkout repository
      uses: actions/checkout@v4
      
    - name: Check documentation exists
      run: |
        test -f README_USB_STORAGE.md
        test -f IMPLEMENTATION_STATUS.md
        test -f DEVELOPMENT_SUMMARY.md
        
    - name: Check for TODO markers
      run: |
        echo "Checking for TODO markers in code..."
        grep -r "TODO\|FIXME\|XXX" bdk/usb/ nyx/nyx_gui/emummc_storage_usb.c || true
        
    - name: Validate markdown
      uses: nosborn/github-action-markdown-cli@v3.3.0
      with:
        files: .
        config_file: .markdownlint.json
```

### Workflow 3: Code Coverage (Future)

**File**: `.github/workflows/code-coverage.yml`

```yaml
name: Code Coverage

on:
  push:
    branches: [ main, develop ]
  pull_request:
    branches: [ main, develop ]

jobs:
  coverage:
    runs-on: ubuntu-latest
    
    steps:
    - name: Checkout repository
      uses: actions/checkout@v4
      
    - name: Install gcov/lcov
      run: sudo apt-get install -y lcov
      
    # This would require modifying the build system to support coverage
    # and creating unit tests
    - name: Build with coverage
      run: |
        echo "Coverage support not yet implemented"
        echo "Requires unit test framework"
        
    - name: Generate coverage report
      run: |
        echo "Future: Generate and upload coverage report"
```

### Workflow 4: Integration Test (Hardware)

**File**: `.github/workflows/hardware-test.yml`

```yaml
name: Hardware Integration Test

on:
  workflow_dispatch:
    inputs:
      device_id:
        description: 'Device serial number'
        required: true
      test_type:
        description: 'Test type'
        required: true
        type: choice
        options:
          - basic
          - read-write
          - performance
          - stress

jobs:
  hardware-test:
    runs-on: self-hosted  # Requires runner with Switch hardware
    
    steps:
    - name: Checkout repository
      uses: actions/checkout@v4
      
    - name: Build with debug enabled
      run: |
        make clean
        make USB_DEBUG=1
        
    - name: Deploy to device
      run: |
        # Copy to SD card or deploy via network
        echo "Deploying to device ${{ github.event.inputs.device_id }}"
        
    - name: Run tests
      run: |
        # This would run automated tests on the hardware
        # Could use serial logging to capture results
        echo "Running ${{ github.event.inputs.test_type }} tests"
        
    - name: Collect logs
      if: always()
      run: |
        # Collect serial logs or SD card logs
        echo "Collecting test results"
        
    - name: Upload test results
      if: always()
      uses: actions/upload-artifact@v3
      with:
        name: hardware-test-results
        path: test-results/
```

## Manual Testing Checklist

### Basic Functionality Tests

Create `tests/manual/USB_BASIC_TESTS.md`:

```markdown
# USB Host Basic Tests

## Test 1: Device Detection
- [ ] Connect USB device through powered hub
- [ ] Boot Hekate
- [ ] Check Nyx logs for device detection
- [ ] Verify device enumeration

Expected: Device detected, slot allocated

## Test 2: Device Information
- [ ] Navigate to USB storage menu in Nyx
- [ ] View device information
- [ ] Check capacity, vendor, product

Expected: Correct device information displayed

## Test 3: Read Test (Small)
- [ ] Read single sector (LBA 0)
- [ ] Verify data integrity
- [ ] Compare with PC read

Expected: Data matches

## Test 4: Read Test (Large)
- [ ] Read 1MB sequential data
- [ ] Measure transfer speed
- [ ] Verify data integrity

Expected: Speed > 5 MB/s, data correct

## Test 5: Cache Test
- [ ] Read same sector twice
- [ ] Verify second read is faster (cache hit)

Expected: Cache working, speed increase

## Test 6: Write Test (if enabled)
- [ ] Enable write mode in config
- [ ] Write test pattern to sector
- [ ] Read back and verify
- [ ] Restore original data

Expected: Write successful, data verified
```

### Compatibility Test Matrix

Create `tests/manual/DEVICE_COMPATIBILITY.md`:

```markdown
# Device Compatibility Test Matrix

## USB Flash Drives

| Vendor | Model | Capacity | USB Ver | Status | Notes |
|--------|-------|----------|---------|--------|-------|
| SanDisk | Cruzer | 32GB | 2.0 | ⏳ | Not tested |
| Kingston | DataTraveler | 64GB | 3.0 | ⏳ | Not tested |
| Samsung | BAR Plus | 128GB | 3.1 | ⏳ | Not tested |

## External SSDs

| Vendor | Model | Capacity | Interface | Status | Notes |
|--------|-------|----------|-----------|--------|-------|
| Samsung | T5 | 500GB | USB 3.1 | ⏳ | Not tested |
| SanDisk | Extreme | 1TB | USB 3.1 | ⏳ | Not tested |

## External HDDs

| Vendor | Model | Capacity | Power | Status | Notes |
|--------|-------|----------|-------|--------|-------|
| WD | My Passport | 2TB | Bus | ⏳ | Not tested |
| Seagate | Backup Plus | 4TB | Adapter | ⏳ | Not tested |

## Test Results Template

For each device, document:
- ✅ Detected
- ✅ Enumerated
- ✅ Capacity read correctly
- ✅ Read test passed
- ✅ Write test passed (if enabled)
- ⚠️ Issues found
- 📝 Notes
```

## Performance Benchmarking

Create `tests/performance/USB_BENCHMARK.md`:

```markdown
# USB Performance Benchmark

## Sequential Read Test
```
Device: [Name]
USB Version: [2.0/3.0/3.1]
Test Size: 100 MB
Block Size: 64 KB

Results:
- Min Speed: ___ MB/s
- Max Speed: ___ MB/s
- Avg Speed: ___ MB/s
- Cache Hit Rate: ___%
```

## Random Read Test
```
Test Pattern: Random 4KB reads
Total Operations: 1000
Test Duration: ___ seconds

Results:
- IOPS: ___
- Avg Latency: ___ ms
```

## Sequential Write Test (if enabled)
```
Test Size: 100 MB
Block Size: 64 KB

Results:
- Min Speed: ___ MB/s
- Max Speed: ___ MB/s
- Avg Speed: ___ MB/s
```
```

## Automated Test Scripts

### Script 1: Build Verification

Create `tests/scripts/verify_build.sh`:

```bash
#!/bin/bash
# Build verification script

set -e

echo "=== Hekate USB Host Build Verification ==="

# Check for required tools
command -v arm-none-eabi-gcc >/dev/null 2>&1 || {
    echo "Error: arm-none-eabi-gcc not found"
    exit 1
}

# Build bootloader
echo "Building bootloader..."
make clean
make -j$(nproc)

# Build Nyx
echo "Building Nyx..."
cd nyx
make clean
make -j$(nproc)
cd ..

# Check outputs exist
echo "Checking outputs..."
test -f output/hekate.bin || { echo "Error: hekate.bin not found"; exit 1; }
test -f nyx/output/nyx.bin || { echo "Error: nyx.bin not found"; exit 1; }

# Check USB files are included
echo "Checking USB object files..."
test -f nyx/build/nyx/xhci.o || { echo "Error: xhci.o not built"; exit 1; }
test -f nyx/build/nyx/usb_msc_host.o || { echo "Error: usb_msc_host.o not built"; exit 1; }

echo "✅ Build verification passed!"
```

### Script 2: Code Quality Check

Create `tests/scripts/check_code_quality.sh`:

```bash
#!/bin/bash
# Code quality check script

echo "=== Code Quality Checks ==="

# Check for common issues
echo "Checking for common issues..."

# Check for TODO/FIXME markers
echo "TODO/FIXME markers:"
grep -rn "TODO\|FIXME" bdk/usb/*.c nyx/nyx_gui/emummc_storage_usb.c || echo "None found"

# Check for magic numbers
echo "Checking for magic numbers..."
grep -n "[^A-Z_][0-9][0-9][0-9]" bdk/usb/*.c | grep -v "Copyright\|line\|//" || echo "Clean"

# Check for memory leaks (simple check)
echo "Checking for malloc without free..."
# This is a simple heuristic
grep -c "malloc\|calloc" bdk/usb/*.c
grep -c "free" bdk/usb/*.c

echo "✅ Code quality check complete"
```

## CI/CD Integration Recommendations

### 1. Pre-commit Hooks
Install hooks to run before each commit:
- Code formatting check
- Basic syntax validation
- TODO marker detection

### 2. Pull Request Checks
Require passing:
- Build check
- Static analysis
- Documentation updates

### 3. Automated Release
On successful tests:
- Generate release binaries
- Create release notes
- Tag version

## Future Enhancements

### Unit Testing Framework
Consider adding:
- **Unity Test Framework** - C unit testing
- **CMock** - Mocking framework for C
- **FFF** - Fake Function Framework

### Hardware-in-Loop Testing
- Dedicated test hardware
- Automated device connection/disconnection
- Serial log capture and analysis
- Automated pass/fail determination

### Fuzz Testing
- Generate random USB descriptors
- Test error handling paths
- Discover edge cases

## Summary

This testing framework provides:
1. ✅ Automated build checks (GitHub Actions)
2. ✅ Static analysis (cppcheck, clang-tidy)
3. ✅ Manual test procedures (checklists)
4. ✅ Performance benchmarking (templates)
5. ✅ Device compatibility tracking (matrix)

To implement:
1. Create `.github/workflows/` directory
2. Add workflow files
3. Configure self-hosted runner (for hardware tests)
4. Create test documentation in `tests/` directory
5. Add pre-commit hooks

This ensures code quality and device compatibility as the implementation progresses.
