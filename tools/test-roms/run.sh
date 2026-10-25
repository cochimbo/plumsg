#!/usr/bin/env bash
# Runs every test ROM of roms.txt with plumsg-headless and keeps what each one showed.
#
#   run.sh HEADLESS ROMS_DIR OUT_DIR
#
# ROMS_DIR is where the downloads are kept (fetched once, reused by later runs);
# OUT_DIR gets one folder per ROM: frame_NNNNNN.png every 300 frames, last.png, the
# hash of every frame (hashes.txt) and the run's summary (run.json).
set -u
headless=$(realpath "$1")
roms=$2
out=$3
here=$(dirname "$(realpath "$0")")
mkdir -p "$roms" "$out"

rom_exts='md|gen|bin|smd|32x|sms|gg|sg'

while IFS='|' read -r name issue url frames script; do
    name=$(echo "$name" | xargs)
    [[ -z "$name" || "$name" == \#* ]] && continue
    issue=$(echo "$issue" | xargs)
    url=$(echo "$url" | xargs)
    frames=$(echo "$frames" | xargs)
    file="$roms/$name/$(basename "$url")"
    if [[ ! -s "$file" ]]; then
        mkdir -p "$roms/$name"
        if ! curl -sSfL --retry 3 -o "$file" "$url"; then
            echo "::warning::$name: could not download $url"
            continue
        fi
        if [[ "$file" == *.zip ]]; then
            (cd "$roms/$name" && unzip -qo "$(basename "$file")")
        fi
    fi
    # The ROMs to run: the download itself, or each ROM in the zip.
    mapfile -t games < <(if [[ "$file" == *.zip ]]; then
        find "$roms/$name" -type f | grep -iE "\.($rom_exts)$" | sort
    else
        echo "$file"
    fi)
    if [[ ${#games[@]} -eq 0 ]]; then
        echo "::warning::$name: no ROM in $(basename "$url")"
        continue
    fi
    for game in "${games[@]}"; do
        run="$name"
        [[ ${#games[@]} -gt 1 ]] && run="$name/$(basename "$game")"
        dir="$out/$run"
        mkdir -p "$dir"
        args=(--frames "$frames" --png-every 300 --out-dir "$dir" --png "$dir/last.png"
              --hash-log "$dir/hashes.txt" --json "$dir/run.json" --quiet)
        IFS=';' read -ra lines <<< "$script"
        for line in "${lines[@]}"; do
            line=$(echo "$line" | xargs)
            [[ -n "$line" ]] && args+=(--do "$line")
        done
        echo "== $run ($issue)"
        timeout 600 "$headless" "$game" "${args[@]}" > "$dir/log.txt" 2>&1
        echo "$?" > "$dir/exit-code.txt"
    done
done < "$here/roms.txt"
