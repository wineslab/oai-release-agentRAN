#!/bin/bash

[[ $# -lt 1 ]] && { echo "Usage: $0 <distance_in_meters...>"; exit 1; }
DISTANCES=("$@")

IP=192.168.71.150
PORT=8091
NCAT_TIMEOUT=1 #s
SLEEP_WAIT=4 #s
MAX_RETRIES=3

set_and_verify_distance() {
  local distance=$1
  echo "Testing PRS ToA estimation for distance: $distance m"

  # it seems that grep returns immediately with this syntax, but not echo | ncat | grep
  # so prefer this to receive new distance immediately. We use --idle to keep
  # ncat open for some additional time
  local setdist_resp="$(grep --max-count 1 new_offset <(echo rfsimu setdistance rfsimu_channel_enB0 $distance | ncat --idle ${NCAT_TIMEOUT} ${IP} ${PORT}))"
  echo "> response: ${setdist_resp}"

  local gettoa_resp="$(echo "ciUE get_max_dl_toa" | ncat ${IP} ${PORT} | grep "UE max PRS DL ToA")"
  echo "> response: ${gettoa_resp}"

  [[ -z "$setdist_resp" || -z "$gettoa_resp" ]] && return 1

  # Extract ToA values 
  [[ "$setdist_resp" =~ new_offset\ ([0-9]+) ]] && local set_toa="${BASH_REMATCH[1]}" || { echo "> Set ToA extraction failed for distance: $distance m"; return 1; }

  [[ "$gettoa_resp" =~ UE\ max\ PRS\ DL\ ToA\ ([0-9]+) ]] && local est_toa="${BASH_REMATCH[1]}" || { echo "> Estimated ToA extraction failed for distance: $distance m"; return 1; }

  # Compare extracted ToA values
  [[ $set_toa == $est_toa ]] && echo "PRS SUCCESS for distance: $distance m" || { echo "PRS FAILURE for distance: $distance m (Actual ToA=$set_toa, Estimated ToA=$est_toa)" ; return 1; }

}

test_distance() {
  local distance=$1 retries=0

  # retry loop incase we didn't receive the response
  while (( retries < MAX_RETRIES )); do
    echo "  Attempt $((retries + 1))/$MAX_RETRIES"

    # Always reset to 0 m before testing target distance
    if ! set_and_verify_distance 0; then
      echo " Set distance 0 m failed during attempt $((retries + 1))"
    else
      sleep "$SLEEP_WAIT"
      # Now test the actual target distance
      if set_and_verify_distance "$distance"; then
        return 0
      fi
    fi

    ((retries++))
    if (( retries < MAX_RETRIES )); then
      sleep "$SLEEP_WAIT"
    else
      echo " ERROR: No valid response after $MAX_RETRIES retries for distance: $distance m"
      return 1
    fi
  done
}

num_success=0
num_fail=0

for d in "${DISTANCES[@]}"; do
  if test_distance "$d"; then
    ((num_success++))
  else
    ((num_fail++))
  fi
  sleep "$SLEEP_WAIT"
done

# ---- Summary ----
echo
echo "==================== SUMMARY ===================="
echo "Total tests run : ${#DISTANCES[@]}"
echo "Successful tests: ${num_success}"
echo "Failed tests    : ${num_fail}"
echo "================================================="

if [ $num_success -gt 0 ]; then
  exit 0
else
  exit 1
fi
