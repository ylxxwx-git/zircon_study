#!/bin/bash

# --- Configuration ---
IMAGE_NAME="arm-os-builder"
CONTAINER_NAME="arm-os-compiler"

# Get the absolute path of the current directory (where the OS source code is)
SOURCE_DIR="$(pwd)"

# 1. Build the Docker image
echo "--- 1. Building Docker Image: $IMAGE_NAME ---"
# The build context is the current directory, using the Dockerfile
docker build -t $IMAGE_NAME .

# Check if the build was successful
if [ $? -ne 0 ]; then
    echo "Docker image build failed. Exiting."
    exit 1
fi

# 2. Run the build container
echo "--- 2. Running Cross-Compilation inside the container ---"
echo "Mounting local directory '$SOURCE_DIR' to '/usr/src/os' inside the container."
echo "Executing 'make all' command..."

# Run the container with the local source directory mounted to /usr/src/os
# The -it flag allows interactive execution and seeing the output
# The --rm flag automatically removes the container after it exits
# The final command is what executes your OS's build process (usually 'make')
docker run --rm \
    -v "$SOURCE_DIR":/usr/src/os \
    --name $CONTAINER_NAME \
    $IMAGE_NAME \
    /bin/bash -c "make all"

# You can replace "make all" with any custom build command needed
# For debugging, you can run:
# docker run -it -v "$SOURCE_DIR":/usr/src/os $IMAGE_NAME /bin/bash
# ... then manually run 'make' inside the container.

if [ $? -eq 0 ]; then
    echo "--- Build successful! Compiled artifacts are in your local directory. ---"
else
    echo "--- Build failed! Check the output above for errors. ---"
fi
