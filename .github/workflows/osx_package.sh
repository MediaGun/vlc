#!/bin/bash
set -ex

SCRIPTDIR=$(dirname "$0")

# Checks whether the file is an executable binary
is_binary() {

    if [ -L "$1" ]; then
        return 1
    fi

    type=$(file "$1")
    if [[ "$type" == *"Mach-O"*  ]]; then
        return 0
    else
        return 1
    fi
}

# compares the hashes of both files and prints a warning if they do not match
hash_compare() {

    if [ -L "$1" ]; then
        return
    fi

    hash1=$(shasum "$1" | awk '{ print $1 }')
    hash2=$(shasum "$2" | awk '{ print $1 }')

    if [ "$hash1" != "$hash2" ]; then
        echo "WARN: Hash differences: $1 ($hash1) versus $2 ($hash2)"
    fi
}

extract_source() {
    mkdir -p $1
    pushd $1
    tar -xvf ../src/$1/vlc-macos-sdk-4.0.0-dev.tar.gz
    popd
}
# Main logic

INTELROOT="x86_64"
ARMROOT="aarch64"
OUTPUT="universal"

extract_source $INTELROOT
extract_source $ARMROOT

echo "Creating universal build in $OUTPUT"
rm -Rf "$OUTPUT"

# Generate directory structure
find "$INTELROOT" -type d -print0 | while IFS= read -r -d '' filePath; do
    relativePath=${filePath#"$INTELROOT"}

    targetFilePath="${OUTPUT}${relativePath}"
    #echo "Create directory $targetFilePath"
    install -d "$targetFilePath"
done

find "$ARMROOT" -type d -print0 | while IFS= read -r -d '' filePath; do
    relativePath=${filePath#"$ARMROOT"}

    targetFilePath="${OUTPUT}${relativePath}"

    #echo "Create directory $targetFilePath"
    install -d "$targetFilePath"
done

# Iterate over all files in INTELROOT directory
find "$INTELROOT" \( -type f -or -type l \) -print0 | while IFS= read -r -d '' filePath; do
    relativePath=${filePath#"$INTELROOT"}

    alternativeFilePath="${ARMROOT}/${relativePath}"
    targetFilePath="${OUTPUT}${relativePath}"

    #echo "Analyzing file $relativePath"

    if [ ! -e "$alternativeFilePath" ]; then
        echo "File only exists in first app, copying file $relativePath"
        cp -a "$filePath" "$targetFilePath"
        continue
    fi

    if is_binary "$filePath"; then
        #echo "Create lipo at $targetFilePath"
        lipo -create -output "$targetFilePath" "$filePath" "$alternativeFilePath"
        continue
    fi

    #echo "Copying file $relativePath"
    #hash_compare "$filePath" "$alternativeFilePath"
    cp -a "$filePath" "$targetFilePath"
done


# Search for files only existing in ARMROOT
find "$ARMROOT" \( -type f -or -type l \) -print0 | while IFS= read -r -d '' filePath; do
    relativePath=${filePath#"$ARMROOT"}

    targetFilePath="${OUTPUT}${relativePath}"

    if [ ! -e "$targetFilePath" ]; then
        echo "File only exists in second app, copying file $relativePath"

        cp -a "$filePath" "$targetFilePath"
    fi

done

pushd $OUTPUT
  tar -cvzf ../vlc-macos-sdk-4.0.0-universal-dev.tar.gz *
popd
