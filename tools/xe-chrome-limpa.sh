#!/bin/zsh
# xe-chrome-limpa: moves Chrome's GPU shader/pipeline caches aside (Graphite/Dawn, GPUCache) and restarts Chrome.
# Chrome only drops these caches when the Metal driver version changes; ours always reports 16.0.32, so pipelines
# compiled before a driver/libxedrv change (or written during a GPU hang) can survive and break the UI (missing icons,
# logo drawn as a bar). The caches are kept in ~/xe/build/chrome-cache-backup-<time>, not deleted.
D="$HOME/Library/Application Support/Google/Chrome"
B=~/xe/build/chrome-cache-backup-$(date +%Y%m%d-%H%M%S)
osascript -e 'quit app "Google Chrome"' 2>/dev/null
for i in {1..10}; do pgrep -x "Google Chrome" >/dev/null || break; sleep 1; done
pgrep -x "Google Chrome" >/dev/null && { echo "Chrome did not quit; close it and run again"; exit 1; }
mkdir -p "$B"
find "$D" -maxdepth 2 -type d \( -name GPUCache -o -name GrShaderCache -o -name ShaderCache -o -name GraphiteDawnCache \
  -o -name DawnGraphiteCache -o -name DawnWebGPUCache \) 2>/dev/null | while read p; do
  n=$(echo "${p#$D/}" | tr '/' '_'); mv "$p" "$B/$n" && echo "moved ${p#$D/}"
done
[[ ${1:-} == --no-open ]] || open -a "Google Chrome"
