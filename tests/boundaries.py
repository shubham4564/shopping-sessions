"""Additional regressions for chunk boundaries and lossless short/long ID handling."""
import datetime as dt
import json
from pathlib import Path
import subprocess
import sys
import tempfile

import polars as pl


def write(path, header, records):
    path.write_bytes((header + "\n" + "\n".join(records)).encode())


def main():
    executable = str(Path(sys.argv[1]).resolve())
    with tempfile.TemporaryDirectory() as directory:
        root = Path(directory)
        data, out = root / "data", root / "output"
        for name in ("events", "clicks", "links"):
            (data / name).mkdir(parents=True)
        (data / "manifest.json").write_text(json.dumps({
            "window_start": "2028-02-29T00:00:00Z", "window_end": "2028-03-12T00:00:00Z",
        }))
        write(data / "products.csv", "product_id,category", ["p,alpha"])
        write(data / "campaigns.csv", "campaign_id,channel", ["c_1,one", "c_2,two"])
        ids = [f"extended-alias-{i}" for i in range(650)] + ["é" * 8, "short"]
        links = [f"{a},{b}" for a, b in zip(ids, ids[1:])] + [f"\0name,{ids[0]}"]
        for part in range(3):
            write(data / "links" / f"{part}.csv", "user_id_a,user_id_b", links[part::3][::-1])
        event_header = "event_id,user_id,ts,ingest_ts,event_type,product_id,price_cents"
        t = "2028-02-29T00:00:00Z"
        # Place the next record exactly on a 1 MiB boundary, then span two more
        # boundaries with a valid non-purchase field containing literal CR.
        prefix = f"prefix,short,{t},{t},view,p,"
        padding = (1 << 20) - len(event_header) - len(prefix) - 2
        later = "2028-03-09T00:00:00Z"
        epoch = int(dt.datetime.fromisoformat(t).timestamp()) * 1000
        write(data / "events" / "a.csv", event_header, [
            prefix + "x" * padding,
            f"long-event-id,short,{t},{t},view,p," + "z" * (2 << 20) + "\r",
            f"purchase1,{ids[0]}," + "0" * 80 + str(epoch) + f",{t},purchase,p," + "0" * 80 + "3",
            f"purchase2,{ids[-2]},{t},{t},purchase,missing,3",
            f"ten-digit-id,short,{later},{later},cart,p,",
            f"too-old-to-rescue,short,{t},{t},purchase,p,２",
            f"spaces, short ,{t},{t},view,p,",
        ])
        write(data / "events" / "b.csv", event_header, [
            f"too-old-to-rescue,other,{t},2028-02-29T00:00:01Z,purchase,p,82",
            f'bad"quote,short,{t},{t},purchase,p,10000',
            f"bad-date,short,2028-02-30T00:00:00Z,{t},purchase,p,10000",
            f"overflow,short," + "9" * 1000 + f",{t},purchase,p,10000",
            f"empty-interior,short,{t},{t},view,p,broken\"\"quote",
        ])
        write(data / "clicks" / "0.csv", "click_id,user_id,campaign_id,ts", [
            f"aaaaaaaaa,short,c_1,{t}", f"aaaaaaaaaa,{ids[1]},c_2,{t}",
            f'"aaaaaaaaa",short,c_1,{t}', f"éé,{ids[2]},c_2,{t}",
            f"123456789012,short,c_2,{later}",
        ])
        subprocess.run([executable, data, out], check=True)
        def records(table, order):
            return pl.read_parquet(out / table / "*.parquet").select(order).sort(order[:2]).rows()
        d1, d2 = dt.date(2028, 2, 29), dt.date(2028, 3, 9)
        assert records("user_daily", ["user_id", "day", "sessions", "events", "revenue_cents", "top_category"]) == [
            ("\0name", d1, 1, 4, 6, "alpha"), ("\0name", d2, 1, 1, 0, None),
            (" short ", d1, 1, 1, 0, None),
        ]
        assert records("campaign_daily", ["campaign_id", "day", "channel", "sessions", "purchasing_sessions",
                                          "revenue_cents", "assisted_sessions"]) == [
            ("c_1", d1, "one", 1, 1, 6, 1), ("c_2", d1, "two", 0, 0, 0, 1),
            ("c_2", d2, "two", 1, 0, 0, 1), ("organic", d1, "organic", 1, 0, 0, 0),
        ]
    print("Chunk, arbitrary-ID, and attribution regressions passed.")


if __name__ == "__main__":
    main()
