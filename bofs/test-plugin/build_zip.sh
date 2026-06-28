#!/bin/bash
# Build test-plugin.zip for Ghost plugin upload testing
# Usage: cd bofs/test-plugin && ./build_zip.sh

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

OUTPUT="test-plugin.zip"

# Remove old archive
rm -f "$OUTPUT"

# Create zip with manifest.json and bofs/ directory
zip -r "$OUTPUT" manifest.json bofs/*.o

echo "Created: $OUTPUT"
echo "Contents:"
unzip -l "$OUTPUT"
