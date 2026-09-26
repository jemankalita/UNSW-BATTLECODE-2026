"""Batch match runner: every map, both sides, in parallel, in the judge sandbox.

Run with the unswbc tool's own Python so its engine and sandbox import:

    & "$env:APPDATA\\uv\\tools\\unswbc\\Scripts\\python.exe" tools/arena.py leviathan starter
    ... tools/arena.py leviathan old_bot --maps arena devil --jobs 6

For each game it reports the winner, how the game ended, and each team's
longest dragon and total length at the end -- the numbers the 500-round
tiebreak actually uses -- so a change can be judged by more than win/loss.
"""

from __future__ import annotations

import argparse
import collections
import multiprocessing
import pathlib
import sys
import time

ROOT = pathlib.Path(__file__).resolve().parent.parent
MAPS_DIR = ROOT / "maps"


def build(bot_dir: str) -> str:
    from unswbc import clangtool

    return str(clangtool.build(pathlib.Path(bot_dir)))


def play(job: tuple[str, str, str, str, str]) -> dict:
    """Run one game, retrying if the sandbox itself fails to start."""
    for attempt in range(3):
        try:
            return play_once(job)
        except Exception:  # sandbox start-up is flaky under heavy parallel load
            if attempt == 2:
                raise
            time.sleep(2)
    raise RuntimeError("unreachable")


def play_once(job: tuple[str, str, str, str, str]) -> dict:
    """Run one game. Team A is wasm_a. Returns a summary dict."""
    map_path, wasm_a, wasm_b, label_a, label_b, seed = job
    from unswbc.engine import EngineModule
    from unswbc.sandbox import SandboxBot, WasmPool

    try:  # unswbc >= 1.1 names the pool's random stream 'key'
        pools = {"A": WasmPool([wasm_a], key="a"), "B": WasmPool([wasm_b], key="b")}
    except TypeError:
        pools = {"A": WasmPool([wasm_a], team="a"), "B": WasmPool([wasm_b], team="b")}
    live: dict[int, SandboxBot] = {}
    teams: dict[int, str] = {}
    lengths: dict[int, int] = {}
    deaths: collections.Counter = collections.Counter()
    peak_points = {"A": 0, "B": 0}
    errors: list[str] = []
    last_label: dict[int, str] = {}
    why: collections.Counter = collections.Counter()

    def spawn(dragon_id: int, init: bytes) -> None:
        team = next((l.split()[1] for l in init.decode().splitlines() if l.startswith("TEAM")), "A")
        teams[dragon_id] = team
        live[dragon_id] = SandboxBot(pools[team], init=init, name=str(dragon_id))

    def reply(dragon_id: int, block: bytes) -> bytes:
        for line in block.split(b"\n", 8):
            if line.startswith(b"LENGTH "):
                lengths[dragon_id] = int(line.split()[1])
                break
        bot = live[dragon_id]
        out = bot.ask(block)
        for line in (out or b"").splitlines():
            if line.startswith(b"INDICATOR "):
                last_label[dragon_id] = line[10:].decode(errors="replace").strip()
        if bot.error is not None and len(errors) < 5:
            errors.append(f"{teams.get(dragon_id)}{dragon_id}: {bot.error}")
        metrics = getattr(bot, "live", None)
        if metrics and metrics[0]:
            team = teams.get(dragon_id, "A")
            peak_points[team] = max(peak_points[team], metrics[0])
        return out

    def death(dragon_id: int, round_num: int, reason: str) -> None:
        deaths[(teams.get(dragon_id, "?"), reason)] += 1
        why[(teams.get(dragon_id, "?"), reason, last_label.get(dragon_id, "?"))] += 1
        lengths.pop(dragon_id, None)
        bot = live.pop(dragon_id, None)
        if bot is not None:
            bot.stop()

    engine = EngineModule()
    try:
        try:  # unswbc >= 1.1 takes a seed, so one map gives many different games
            result = engine.run(pathlib.Path(map_path).read_bytes(), reply, death, spawn, lambda _l: None, 0, seed)
        except TypeError:
            result = engine.run(pathlib.Path(map_path).read_bytes(), reply, death, spawn, lambda _l: None, 0)
    finally:
        for bot in live.values():
            bot.stop()
        for pool in pools.values():
            pool.close()

    def team_stats(team: str) -> tuple[int, int, int]:
        mine = [length for dragon, length in lengths.items() if teams.get(dragon) == team]
        return (max(mine) if mine else 0, sum(mine), len(mine))

    return {
        "map": pathlib.Path(map_path).stem + (f"#{seed}" if seed else ""),
        "a": label_a,
        "b": label_b,
        "winner": result.winner,
        "rounds": result.rounds + 1,
        "reason": "elim" if result.end_reason == 0 else "length",
        "stats": {"A": team_stats("A"), "B": team_stats("B")},
        "deaths": dict(deaths),
        "points": peak_points,
        "errors": errors,
        "why": dict(why),
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("bot")
    parser.add_argument("opponent")
    parser.add_argument("--maps", nargs="*", help="map names (stems), .map paths or folders; default: all in maps/")
    parser.add_argument("--jobs", type=int, default=6)
    parser.add_argument("--one-side", action="store_true", help="only play with bot as team A")
    parser.add_argument("--why", action="store_true", help="tally our deaths by the dragon's last indicator")
    parser.add_argument("--seeds", type=int, default=1, help="games per map and side, each with its own engine seed")
    args = parser.parse_args()

    if args.maps:
        maps = []
        for m in args.maps:
            if pathlib.Path(m).is_dir():
                maps += sorted(str(p) for p in pathlib.Path(m).glob("*.map"))
            else:
                maps.append(str(MAPS_DIR / f"{m}.map") if not m.endswith(".map") else m)
    else:
        maps = sorted(str(p) for p in MAPS_DIR.glob("*.map"))

    wasm_bot = build(args.bot)
    wasm_opp = build(args.opponent)
    label_bot = pathlib.Path(args.bot).name
    label_opp = pathlib.Path(args.opponent).name

    jobs = []
    for map_path in maps:
        for seed in range(args.seeds):
            jobs.append((map_path, wasm_bot, wasm_opp, label_bot, label_opp, seed))
            if not args.one_side:
                jobs.append((map_path, wasm_opp, wasm_bot, label_opp, label_bot, seed))

    started = time.monotonic()
    with multiprocessing.Pool(args.jobs) as pool:
        results = pool.map(play, jobs, chunksize=1)

    wins = losses = draws = 0
    print(f"{'map':<18}{'side':<5}{'result':<8}{'how':<8}{'rnd':>4}  {'us max/total/n':>16}  {'them max/total/n':>16}  deaths(us|them)")
    for res in results:
        side = "A" if res["a"] == label_bot else "B"
        other = "B" if side == "A" else "A"
        if res["winner"] is None:
            outcome = "draw"
            draws += 1
        elif res["winner"] == side:
            outcome = "WIN"
            wins += 1
        else:
            outcome = "loss"
            losses += 1
        us, them = res["stats"][side], res["stats"][other]
        ours = ",".join(f"{r}{n}" for (t, r), n in sorted(res["deaths"].items()) if t == side)
        theirs = ",".join(f"{r}{n}" for (t, r), n in sorted(res["deaths"].items()) if t == other)
        print(f"{res['map']:<18}{side:<5}{outcome:<8}{res['reason']:<8}{res['rounds']:>4}  "
              f"{us[0]:>5}/{us[1]:>4}/{us[2]:>3}    {them[0]:>5}/{them[1]:>4}/{them[2]:>3}    {ours or '-'} | {theirs or '-'}")
        for error in res["errors"]:
            print(f"    ! {error}")
    if args.why:
        tally: collections.Counter = collections.Counter()
        for res in results:
            side = "A" if res["a"] == label_bot else "B"
            for (team, reason, label), count in res["why"].items():
                if team == side:
                    tally[(reason, label)] += count
        print("\nour deaths by (reason, last indicator):")
        for (reason, label), count in tally.most_common():
            print(f"  {reason} {label:<12} {count}")
    print(f"\n{label_bot} vs {label_opp}: {wins}W {losses}L {draws}D  "
          f"({time.monotonic() - started:.0f}s)   death codes: W wall, S self, O other body, H head-to-head, A no action")
    return 0


if __name__ == "__main__":
    sys.exit(main())
