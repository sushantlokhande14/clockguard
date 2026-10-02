"""Just enough VCD reading to draw a few signals around one point in time."""
import html


def read(path, wanted):
    """Value changes for the signals whose full dotted name is in `wanted`.

    Returns {name: (width, [(time, value_string), ...])}, times in the
    file's units.
    """
    ids, scope, out, t = {}, [], {}, 0
    with open(path) as f:
        tokens = iter(f.read().split())
    for tok in tokens:
        if tok == "$scope":
            next(tokens)
            scope.append(next(tokens))
        elif tok == "$upscope":
            scope.pop()
        elif tok == "$var":
            next(tokens)
            width = int(next(tokens))
            code = next(tokens)
            name = next(tokens)
            for x in tokens:  # skip an optional [msb:lsb]
                if x == "$end":
                    break
            full = ".".join(scope + [name])
            if full in wanted:
                ids.setdefault(code, []).append(full)
                out[full] = (width, [])
        elif tok.startswith("#"):
            t = int(tok[1:])
        elif tok[0] in "bBrR":
            code = next(tokens)
            for n in ids.get(code, []):
                out[n][1].append((t, tok[1:]))
        elif tok[0] in "01xXzZ" and len(tok) > 1:
            for n in ids.get(tok[1:], []):
                out[n][1].append((t, tok[0]))
    return out


def svg(signals, order, t0, t1, marks=(), labels=None, unit_ns=0.001):
    """Draw signals between t0 and t1 (file units); `marks` are (time, text)."""
    e = html.escape
    lw, ww, rh, top = 190, 760, 26, 18  # marker labels go in the top strip
    h = top + rh * (len(order) + 1) + 4
    x = lambda t: lw + (t - t0) * ww / max(1, t1 - t0)  # noqa: E731
    p = [f"<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 {lw + ww + 10} {h}' "
         f"font-family='monospace' font-size='11'>",
         f"<rect width='100%' height='100%' fill='#fff'/>"]
    for k, name in enumerate(order):
        y = top + 6 + k * rh
        label = (labels or {}).get(name, name)
        p.append(f"<text x='4' y='{y + 15}' fill='#1d2330'>{e(label[-26:])}</text>")
        width, ch = signals.get(name, (1, []))
        val = "x"
        for tt, v in ch:  # value at t0
            if tt <= t0:
                val = v
        pts = [(t0, val)] + [(tt, v) for tt, v in ch if t0 < tt <= t1]
        for i, (ta, va) in enumerate(pts):
            tb = pts[i + 1][0] if i + 1 < len(pts) else t1
            xa, xb = x(ta), x(tb)
            if width == 1:
                if va in "01":
                    yy = y + (4 if va == "1" else 18)
                    p.append(f"<line x1='{xa:.1f}' y1='{yy}' x2='{xb:.1f}' y2='{yy}' stroke='#1f6feb' stroke-width='1.5'/>")
                    if i + 1 < len(pts):
                        p.append(f"<line x1='{xb:.1f}' y1='{y + 4}' x2='{xb:.1f}' y2='{y + 18}' stroke='#1f6feb' "
                                 f"stroke-width='1'/>")
                else:
                    p.append(f"<rect x='{xa:.1f}' y='{y + 4}' width='{max(0.5, xb - xa):.1f}' height='14' "
                             f"fill='#f6d5d5'/>")
            else:
                text = val_text(va)
                p.append(f"<polygon points='{xa + 2:.1f},{y + 11} {xa + 4:.1f},{y + 4} {xb - 4:.1f},{y + 4} "
                         f"{xb - 2:.1f},{y + 11} {xb - 4:.1f},{y + 18} {xa + 4:.1f},{y + 18}' fill='#eef3fb' "
                         f"stroke='#1f6feb'/>")
                if xb - xa > 7 * len(text) + 6:
                    p.append(f"<text x='{xa + 6:.1f}' y='{y + 15}' fill='#1d2330'>{e(text)}</text>")
    for tm, text in marks:
        if t0 <= tm <= t1:
            p.append(f"<line x1='{x(tm):.1f}' y1='{top - 2}' x2='{x(tm):.1f}' y2='{h - 14}' stroke='#c0392b' "
                     f"stroke-dasharray='4 3'/>")
            right = x(tm) > lw + ww * 0.55  # keep long labels inside the picture
            p.append(f"<text x='{x(tm) + (-4 if right else 4):.1f}' y='12' fill='#c0392b' "
                     f"text-anchor='{'end' if right else 'start'}'>{e(text)}</text>")
    span = (t1 - t0) * unit_ns
    p.append(f"<text x='{lw}' y='{h - 3}' fill='#5f6b7a'>{t0 * unit_ns:.2f} ns</text>")
    p.append(f"<text x='{lw + ww - 70}' y='{h - 3}' fill='#5f6b7a'>+{span:.1f} ns</text>")
    p.append("</svg>")
    return "\n".join(p)


def val_text(v):
    if any(c in v for c in "xXzZ"):
        return "x"
    try:
        return format(int(v, 2), "x")
    except ValueError:
        return v
