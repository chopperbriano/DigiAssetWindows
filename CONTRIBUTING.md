# Contributing to DigiAsset Core Windows Fork

This document provides guidelines and instructions for working with this Windows fork of DigiAsset Core.

## Project Focus

This fork is specifically focused on making the upstream DigiAsset Core compile and work on Windows using Visual Studio and MSVC. We do not intend to develop new features, but rather maintain compatibility with the upstream project while ensuring it works properly on Windows.

## Windows-Specific Changes

The following modifications were made to port DigiAsset Core to Windows:

### Compiler and Language Standard Compatibility
- Made the codebase strictly C++11 compliant for MSVC compatibility
- Removed C++20 designated initializers which were allowed as extensions in GCC/Clang but not in MSVC
- Ensured ABI compatibility between the project and its dependencies by maintaining consistent C++ standard usage
- This was critical as MSVC doesn't maintain ABI compatibility between different C++ standards (e.g., Json::String had different memory layouts when compiled with different standards)
- Resolved compiler and platform specific warnings and errors

### Build System Changes
- Modified CMake configuration to support MSVC compiler and Visual Studio
- Created batch scripts for configuring and building dependencies
- Added Visual Studio solution generation support

### External Dependencies
- Compiled our own version of jsoncpp in order to control the installed "include" path
- Forked and modified libjson-rpc-cpp to ensure Windows compatibility
- Configured vcpkg for managing Windows dependencies
- Ensured all dependencies build properly with MSVC

When contributing to this project, please ensure that any changes maintain compatibility with both the upstream codebase and Windows build environment. If you're modifying code that interacts with the operating system, make sure to test it thoroughly on Windows.

## Repository Structure

This fork maintains two primary branches:

- **main**: Contains the Windows-specific implementation using Visual Studio and MSVC
- **upstream-master**: Tracks the upstream DigiAsset Core repository

## Getting Started

### Full Repository Clone

For development work, clone the complete repository history:

```cmd
git clone --no-single-branch --recurse-submodules https://github.com/chopperbrian/DigiAsset_Core_Windows.git
```

This ensures you have access to both branches and all commit history, which is essential for working with upstream changes.

### Syncing with Upstream

First, ensure you have a tracking branch set up that follows the upstream repository:

1. Add the upstream repository as a remote (if not already done):
   ```cmd
   git remote add upstream https://github.com/DigiAsset-Core/DigiAsset_Core.git
   ```

2. Create a tracking branch for upstream/master:
   ```cmd
   git fetch upstream
   git checkout -b upstream-master --recurse-submodules upstream/master
   ```

3. To update the upstream-master branch with the latest changes:
   ```cmd
   git checkout upstream-master
   git pull
   ```

### Merging Upstream Changes into Main

This workflow is specifically for maintainers of this repository who need to incorporate upstream changes into the Windows implementation.

```cmd
# Create a development branch from main
git checkout -b dev main

# Rebase the development branch onto upstream-master
git rebase upstream-master

# Make any necessary adjustments or fixes to resolve conflicts
# Continue working on the dev branch until everything is working correctly

# Rebase dev onto main (in case main has changed)
git rebase main

# Merge the development branch into main (should be a fast-forward merge)
git checkout main
git merge dev

# Optionally, delete the development branch when done
git branch -d dev
```

**Important:** Do not push the dev branch to the public repository. Keep it private as it might require force pushing during the rebase process. You don't want others to base their work on this temporary branch.

This workflow allows you to integrate upstream changes through rebasing, test and fix any issues on a separate branch, preserve the history of main, and create a clean merge into main.

## Development Workflow

1. Make Windows-specific changes to the `main` branch
2. Test thoroughly on Windows
3. Submit a pull request if appropriate

## Building for Development

Follow the build instructions in the README.md file, but use the full repository clone command to ensure you have access to all branches and history.

## Testing

Before submitting changes, ensure:

1. The application builds successfully on Windows
2. All functionality works as expected
3. Any Windows-specific adaptations are properly documented

## Code Style

- Follow the existing code style in the repository
- Use the provided `.clang-format` file for code formatting

## Submitting Changes

1. Push your changes to your feature branch
2. Create a pull request against the `main` branch
3. Provide a clear description of the changes and their purpose

## Questions and Support

If you have questions or need assistance, please open an issue in the repository.
