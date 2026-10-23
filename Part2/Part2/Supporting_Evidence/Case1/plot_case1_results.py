import csv
import os
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

root = os.path.dirname(os.path.abspath(__file__))
csv_path = os.path.join(root, "Part2_Case1_results.csv")

with open(csv_path, newline="", encoding="utf-8-sig") as f:
    rows = list(csv.DictReader(f))

if not rows:
    raise SystemExit("The Case 1 CSV has no result rows.")

successful = []
excluded = []

for row in rows:
    if row.get("case", "").strip().lower() != "case1":
        continue

    try:
        p = float(row["failure_probability"])
        useful = int(row["useful_bytes"])
        redundant = int(row["redundant_bytes"])
        seconds = float(row["client_seconds"])
    except (ValueError, KeyError, TypeError):
        excluded.append(row)
        continue

    verified = row.get("verification", "").strip().upper() == "PASS"
    status_ok = row.get("status", "").strip().upper() == "OK"

    if not (status_ok and verified and useful > 0 and seconds > 0):
        excluded.append(row)
        continue

    note = row.get("notes", "")
    run = row.get("run", "?")

    if "Client A" in note:
        label = "Client A"
    elif "Client B" in note:
        label = "Client B"
    elif "baseline" in note.lower():
        label = "Baseline"
    else:
        label = f"Run {run}"

    successful.append({
        "p": p,
        "label": label,
        "throughput": useful / seconds / 1_000_000,
        "overhead": redundant / useful * 100,
        "useful": useful,
        "redundant": redundant,
        "seconds": seconds,
    })

if not successful:
    raise SystemExit(
        "No verified successful trials found. "
        "Check status and verification fields in the CSV."
    )

successful.sort(key=lambda r: (r["p"], r["label"]))

labels = [f'p={r["p"]:g}\n{r["label"]}' for r in successful]
x = list(range(len(successful)))

# Graph 1: Useful application throughput
plt.figure(figsize=(11, 5.5))
bars = plt.bar(x, [r["throughput"] for r in successful])
plt.xticks(x, labels, rotation=35, ha="right")
plt.ylabel("Useful throughput (decimal MB/s)")
plt.xlabel("Recorded trial and server failure probability")
plt.title("Part 2 - Case 1: Verified Transfer Throughput")
plt.grid(axis="y", alpha=0.3)
plt.bar_label(bars, fmt="%.3f", padding=3)
plt.tight_layout()

for ext in ("png", "pdf"):
    plt.savefig(
        os.path.join(root, f"Part2_Case1_throughput.{ext}"),
        dpi=200, bbox_inches="tight"
    )
plt.close()

# Graph 2: Retransmission overhead relative to useful bytes
plt.figure(figsize=(11, 5.5))
bars = plt.bar(x, [r["overhead"] for r in successful])
plt.xticks(x, labels, rotation=35, ha="right")
plt.ylabel("Redundant bytes / useful bytes (%)")
plt.xlabel("Recorded trial and server failure probability")
plt.title("Part 2 - Case 1: Retransmission Overhead")
plt.grid(axis="y", alpha=0.3)
plt.bar_label(bars, fmt="%.1f%%", padding=3)
plt.tight_layout()

for ext in ("png", "pdf"):
    plt.savefig(
        os.path.join(root, f"Part2_Case1_overhead.{ext}"),
        dpi=200, bbox_inches="tight"
    )
plt.close()

print(f"CSV read: {csv_path}")
print(f"Verified successful trials plotted: {len(successful)}")
print(f"Failed, incomplete, or unverified rows excluded: {len(excluded)}")
print("\nTrials plotted:")
for r in successful:
    print(
        f'p={r["p"]:g}, {r["label"]}: '
        f'{r["throughput"]:.3f} MB/s, '
        f'overhead={r["overhead"]:.2f}%, '
        f'time={r["seconds"]:.3f}s'
    )

print("\nCreated:")
print("Results/Part2_Case1_throughput.png")
print("Results/Part2_Case1_throughput.pdf")
print("Results/Part2_Case1_overhead.png")
print("Results/Part2_Case1_overhead.pdf")
