"""Exercise vector-sized CSV blocks and multiword assist masks against a Python oracle."""
from collections import Counter, defaultdict
from datetime import datetime, timedelta, timezone
import json
from pathlib import Path
import random
import subprocess
import sys
import tempfile

import polars as pl


DAY = 86_400_000
WEEK = 7 * DAY
GAP = 1_800_000
BASE = datetime(2028, 2, 24, tzinfo=timezone.utc)
EPOCH = int(BASE.timestamp()) * 1000


def date_at(ms):
    return BASE + timedelta(milliseconds=ms - EPOCH)


def iso(ms):
    return date_at(ms).strftime("%Y-%m-%dT%H:%M:%SZ")


def timestamp(ms, i):
    if i % 3 == 0 or ms % 1000:
        return str(ms)
    if i % 3 == 1:
        return iso(ms)
    return (date_at(ms) - timedelta(hours=5, minutes=30)).strftime("%Y-%m-%dT%H:%M:%S-05:30")


def main():
    binary = str(Path(sys.argv[1]).resolve())
    rng = random.Random(489531)
    events, clicks = [], []
    expected_events, expected_clicks = defaultdict(list), defaultdict(list)
    channels = {"c_0": "search", "c_1": "mail", "summer": "display", "organic": "organic"}

    def csv(fields):
        return ",".join(f'"{x}"' if rng.randrange(5) == 0 else x for x in fields)

    def event(identifier, user, person, ts, kind, product, cents, i):
        fields = [identifier, user, timestamp(ts, i), iso(ts), kind, product, str(cents)]
        events.append(csv(fields))
        cat = {"p0": "alpha", "p1": "beta"}.get(product, "unknown")
        expected_events[person].append((ts, cat, cents if kind == "purchase" else 0))
        if i % 17 == 0:
            fields[1], fields[3] = "unrelated", iso(ts + 1000)
            events.append(csv(fields))

    def click(identifier, user, person, ts, ad, i):
        fields = [identifier, user, ad, timestamp(ts, i)]
        clicks.append(csv(fields))
        expected_clicks[person].append((ts, identifier.encode(), ad))
        if i % 11 == 0:
            clicks.append(csv(fields))

    for j in range(178):
        start = EPOCH + DAY - 5 * 60_000 + j * 90 * 60_000
        for k in range(46):
            i = j * 46 + k
            ts = start + (k // 2) * 25_000
            kind = "purchase" if k % 9 == 0 else ("cart" if k % 7 == 0 else "view")
            event(f"e{i:07}", ("u1", "u2")[i % 2], "A", ts, kind,
                  ("p0", "p1", "missing")[i % 3], 2 + k % 3, i)
        for k, offset in enumerate((-WEEK - 1, -WEEK, -1000, 0, 0, 1)):
            ad = ("c_0", "c_1", "summer")[(j + k) % 3]
            click(f"k{j:04}-{k}", "u2", "A", start + offset, ad, j + k)

    for j in range(600):
        user = ("solo", " independent ", "É", "\0x", "123456789", "1234567890")[j % 6]
        person = f"{user}{j // 6}"
        start = EPOCH + (j % 5) * DAY + 20_000
        for k in range(j % 4 + 1):
            event(f"small-{j}-{k}", person, person, start + k * (GAP + 1), "view", "p0", 0, j)
        click(f"long-click-id-{j}", person, person, start, "summer", j)
        click(f"a{j}", person, person, start, "c_1", j)

    # Invalid first candidate suppresses its otherwise valid later copy.
    for j in range(80):
        start = EPOCH + DAY
        events.extend([csv([f"bad-copy{j}", "other", "2028-02-30T00:00:00Z", iso(start),
                            "purchase", "p0", "500"]),
                       csv([f"bad-copy{j}", "u1", iso(start), iso(start + 1000),
                            "purchase", "p0", "500"])])
    rng.shuffle(events)
    rng.shuffle(clicks)
    # Keep many consecutive well-formed blocks so the vector path is exercised.
    ascii_events = [r for r in events if r.isascii() and "\0" not in r]
    events = ascii_events + [r for r in events if not r.isascii() or "\0" in r]
    events += ['bad"quote,u1,x,x,view,p0,', ",,,,,,,", "", "wrong,field,count"]
    expected_users, expected_ads = {}, defaultdict(lambda: [0, 0, 0, 0])
    for person, hits in expected_events.items():
        seen = sorted(hits)
        sessions = []
        for i, (t, cat, cents) in enumerate(seen):
            if i == 0 or t - seen[i - 1][0] > GAP:
                sessions.append((t, []))
            sessions[-1][1].append((cat, cents))
        daily = defaultdict(lambda: [0, 0, Counter()])
        for t, members in sessions:
            day = date_at(t).date()
            row = daily[day]
            row[0] += 1
            row[1] += len(members)
            for cat, cents in members:
                if cents:
                    row[2][cat] += cents
            qualifying = [x for x in expected_clicks[person] if t - WEEK <= x[0] <= t]
            ad = min(qualifying, key=lambda x: (-x[0], x[1]))[2] if qualifying else "organic"
            credit = expected_ads[ad, day]
            revenue = sum(c for _, c in members)
            credit[0] += 1
            credit[1] += int(revenue != 0)
            credit[2] += revenue
            for other in {x[2] for x in qualifying}:
                expected_ads[other, day][3] += 1
        for day, (sessions, count, cats) in daily.items():
            category = min(cats, key=lambda c: (-cats[c], c.encode())) if cats else None
            expected_users[person, day] = (sessions, count, sum(cats.values()), category)

    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        data, out = root / "data", root / "out"
        for folder, header, lines in [
            ("events", "event_id,user_id,ts,ingest_ts,event_type,product_id,price_cents", events),
            ("clicks", "click_id,user_id,campaign_id,ts", clicks),
            ("links", "user_id_a,user_id_b", ["u1,unused", 'unused,"A"', "u2,u1"]),
        ]:
            (data / folder).mkdir(parents=True)
            (data / folder / "rows.csv").write_bytes(("\n".join([header] + lines)).encode())
        (data / "products.csv").write_text("product_id,category\np0,alpha\np1,beta\n")
        (data / "campaigns.csv").write_text("campaign_id,channel\nc_0,search\nc_1,mail\nsummer,display\n")
        for days in (14, 40):
            (data / "manifest.json").write_text(json.dumps({
                "window_start": iso(EPOCH), "window_end": iso(EPOCH + days * DAY),
            }))
            subprocess.run([binary, data, out], check=True)
            u = pl.read_parquet(out / "user_daily" / "*.parquet").select(
                "user_id", "day", "sessions", "events", "revenue_cents", "top_category"
            )
            c = pl.read_parquet(out / "campaign_daily" / "*.parquet").select(
                "campaign_id", "day", "channel", "sessions", "purchasing_sessions", "revenue_cents", "assisted_sessions"
            )
            assert u.height == len(expected_users) and c.height == len(expected_ads)
            assert all(tuple(v) == expected_users[k, d] for k, d, *v in u.rows())
            assert all(channel == channels[k] and v == expected_ads[k, d] for k, d, channel, *v in c.rows())
    print("Dense sessions, click ties, assist windows and wide-manifest regressions passed.")


if __name__ == "__main__":
    main()
