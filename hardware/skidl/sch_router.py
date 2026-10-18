"""Трассировка проводов на листе схемы KiCad: ортогонально, по сетке 1,27 мм.

Правила, которые не дают случайно соединить чужие цепи (в KiCad провод
соединяется с выводом или с другим проводом, если конец одного лежит на
другом):
- корпуса деталей с выводами — запретная зона; конец вывода доступен
  только своей цепи;
- «узел» (конец провода, изгиб, Т-ответвление, конец вывода) занимает
  клетку целиком — через неё не проходит и не пересекает чужая цепь;
- чужие провода можно только пересечь под прямым углом, насквозь, без
  изгиба и ответвления в точке пересечения — KiCad такие провода не
  соединяет.

Цепь разводится деревом: от первого вывода к ближайшему из остальных,
каждый следующий вывод — к ближайшей точке уже проведённой части (A*).
"""

import heapq

G = 1.27                     # шаг сетки, мм
DIRS = [(1, 0), (0, 1), (-1, 0), (0, -1)]          # E, S, W, N (Y вниз)
HORIZ = (0, 2)

COST_STEP, COST_BEND, COST_CROSS, COST_NEAR, COST_SOFT, COST_SIDE_EXIT = 1, 6, 4, 2, 10, 8


def cell(x_mm, y_mm):
    return round(x_mm / G), round(y_mm / G)


class Router:
    def __init__(self, width_mm, height_mm):
        self.W = int(width_mm / G) + 2
        self.H = int(height_mm / G) + 2
        n = self.W * self.H
        self.hard = bytearray(n)       # корпуса, тексты заголовков, рамки
        self.soft = bytearray(n)       # тексты полей (обозначения, номиналы)
        self.h_own = [0] * n           # чья горизонтальная линия идёт через клетку
        self.v_own = [0] * n
        self.node = [0] * n            # чей узел в клетке
        self.bounds = (0, 0, self.W - 1, self.H - 1)

    def idx(self, x, y):
        return y * self.W + x

    # --------------------------------------------------------- препятствия
    def block_rect(self, x0, y0, x1, y1, soft=False):
        """Прямоугольник в мм (включительно)."""
        a, b = cell(min(x0, x1), min(y0, y1))
        c, d = cell(max(x0, x1), max(y0, y1))
        arr = self.soft if soft else self.hard
        for y in range(max(b, 0), min(d, self.H - 1) + 1):
            row = y * self.W
            for x in range(max(a, 0), min(c, self.W - 1) + 1):
                arr[row + x] = 1

    def set_terminal(self, xy, net):
        i = self.idx(*xy)
        self.hard[i] = 0
        self.soft[i] = 0
        self.node[i] = net

    def block_cell(self, xy):
        self.hard[self.idx(*xy)] = 1

    # --------------------------------------------------------- проверки
    def free_for(self, i, net):
        return not self.hard[i] and self.node[i] in (0, net)

    def nodeable(self, i, net):
        """Можно ли поставить здесь узел цепи (изгиб, ответвление, конец)."""
        return (not self.hard[i] and self.node[i] in (0, net)
                and self.h_own[i] in (0, net) and self.v_own[i] in (0, net))

    def can_pass(self, i, net, horiz):
        own, other = (self.h_own, self.v_own) if horiz else (self.v_own, self.h_own)
        if self.hard[i] or self.node[i] not in (0, net) or own[i] not in (0, net):
            return False
        return True

    # --------------------------------------------------------- трассировка
    def route_net(self, net, terminals, exits, area):
        """terminals: [(x, y)] клетки концов выводов; exits: {клетка: (dx, dy)}
        — направление «наружу» от корпуса. area: (x0, y0, x1, y1) в клетках.
        Возвращает (рёбра, узлы) или None для невыведенных выводов."""
        terms = list(dict.fromkeys(terminals))
        tree = {terms[0]}
        edges = set()
        nodes = {terms[0]}
        failed = []
        rest = terms[1:]
        while rest:
            rest.sort(key=lambda t: min(abs(t[0] - c[0]) + abs(t[1] - c[1]) for c in tree))
            t = rest.pop(0)
            path = self._astar(net, t, tree, exits, area)
            if path is None:
                failed.append(t)
                continue
            self._commit(net, path, tree, edges, nodes)
        return edges, nodes, failed

    def _astar(self, net, start, tree, exits, area):
        ax0, ay0, ax1, ay1 = area
        tx, ty = min(tree, key=lambda c: abs(c[0] - start[0]) + abs(c[1] - start[1]))
        W = self.W
        si = self.idx(*start)
        open_ = [(0, 0, si, -1)]
        best = {(si, -1): 0}
        came = {}
        ex = exits.get(start)
        tree_idx = {self.idx(*c) for c in tree}
        while open_:
            f, g, i, d = heapq.heappop(open_)
            if best.get((i, d), 1 << 30) < g:
                continue
            x, y = i % W, i // W
            if i in tree_idx and i != si:
                return self._path(came, (i, d), si)
            for nd, (dx, dy) in enumerate(DIRS):
                if d != -1 and nd == (d + 2) % 4:
                    continue
                nx, ny = x + dx, y + dy
                if not (ax0 <= nx <= ax1 and ay0 <= ny <= ay1):
                    continue
                ni = ny * W + nx
                horiz = nd in HORIZ
                cost = COST_STEP
                if d == -1:
                    if ex and (dx, dy) != ex:
                        cost += COST_SIDE_EXIT
                elif nd != d:
                    if not self.nodeable(i, net):
                        continue
                    cost += COST_BEND
                # клетка i (откуда) — линия в направлении движения
                if d != -1 and not self.can_pass(i, net, horiz):
                    continue
                if ni in tree_idx:
                    # присоединение к дереву: в точке будет узел
                    if not self.nodeable(ni, net) and not (self.node[ni] == net):
                        continue
                else:
                    if not self.can_pass(ni, net, horiz):
                        continue
                    other = self.v_own if horiz else self.h_own
                    if other[ni] not in (0, net):
                        cost += COST_CROSS
                    if self.soft[ni]:
                        cost += COST_SOFT
                    if self._near_foreign(nx, ny, net, horiz):
                        cost += COST_NEAR
                ng = g + cost
                key = (ni, nd)
                if ng < best.get(key, 1 << 30):
                    best[key] = ng
                    came[key] = (i, d)
                    h = abs(nx - tx) + abs(ny - ty)
                    heapq.heappush(open_, (ng + h, ng, ni, nd))
        return None

    def _near_foreign(self, x, y, net, horiz):
        """Параллельная чужая линия в соседней клетке — дороже (читаемость)."""
        W = self.W
        if horiz:
            for yy in (y - 1, y + 1):
                if 0 <= yy < self.H and self.h_own[yy * W + x] not in (0, net):
                    return True
        else:
            for xx in (x - 1, x + 1):
                if 0 <= xx < W and self.v_own[y * W + xx] not in (0, net):
                    return True
        return False

    def _path(self, came, key, si):
        out = [key[0]]
        while key in came:
            key = came[key]
            out.append(key[0])
            if key[0] == si and key[1] == -1:
                break
        return out[::-1]

    def _commit(self, net, path, tree, edges, nodes):
        W = self.W
        cells = [(i % W, i // W) for i in path]
        for k in range(len(cells) - 1):
            a, b = cells[k], cells[k + 1]
            horiz = a[1] == b[1]
            own = self.h_own if horiz else self.v_own
            own[self.idx(*a)] = net
            own[self.idx(*b)] = net
            edges.add((a, b) if a < b else (b, a))
        # узлы: начало, конец (точка присоединения), изгибы
        marks = [cells[0], cells[-1]]
        for k in range(1, len(cells) - 1):
            p, c, n = cells[k - 1], cells[k], cells[k + 1]
            if (c[0] - p[0], c[1] - p[1]) != (n[0] - c[0], n[1] - c[1]):
                marks.append(c)
        for m in marks:
            self.node[self.idx(*m)] = net
            nodes.add(m)
        tree.update(cells)


def segments(edges, nodes):
    """Рёбра сетки -> отрезки проводов от узла до узла (по прямой)."""
    adj = {}
    for a, b in edges:
        adj.setdefault(a, []).append(b)
        adj.setdefault(b, []).append(a)
    stops = set(nodes)
    for c, ns in adj.items():
        if len(ns) != 2:
            stops.add(c)
        else:
            (a, b) = ns
            if (a[0] - c[0], a[1] - c[1]) != (c[0] - b[0], c[1] - b[1]):
                stops.add(c)
    seen = set()
    out = []
    for s in stops:
        for n in adj.get(s, []):
            e = (s, n) if s < n else (n, s)
            if e in seen:
                continue
            seen.add(e)
            cur, prev = n, s
            while cur not in stops:
                nxt = [x for x in adj[cur] if x != prev][0]
                e2 = (cur, nxt) if cur < nxt else (nxt, cur)
                seen.add(e2)
                prev, cur = cur, nxt
            out.append((s, cur))
    return out, adj
