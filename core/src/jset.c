#include "features_internal.h"

/* Java HashSet<BlockPos> 에뮬레이션 — JDK 25 java.util.HashMap 1:1 이식
 * (본 머신 /usr/lib/jvm/java-25-openjdk-amd64 의 HashMap/HashMap$TreeNode
 * javap -p -c 로 검증; removeTreeNode 의 too-small untreeify 가 movable
 * 게이트라는 JDK9+ 시맨틱 포함).
 *
 * 노드 hash 필드 = spread(hashCode) = h ^ (h >>> 16) (putVal 이 spread 를
 * 저장한다); 트리 정렬 비교는 spread 의 SIGNED int 비교. 동일 hashCode
 * 인 서로 다른 키의 트리 순서는 tieBreakOrder → System.identityHashCode
 * — JVM 재현 불가라 도달 시 즉사한다 (본 그리드의 셋 기하에선 동일
 * hashCode 쌍이 한 셋에 못 들어온다 — R3 노트 참조). */

#define die hc_featx_die

enum { NIL = -1 };

#define ENT(s, i) ((s)->ent[(i)])

static uint32_t spread_hash(int32_t x, int32_t y, int32_t z) {
    uint32_t h = (uint32_t)((y + z * 31) * 31 + x); /* Vec3i.hashCode */
    return h ^ (h >> 16);                           /* HashMap.hash() */
}

void hc_jset_init(hc_jset_t *s) {
    s->n_ent = 0;
    s->cap = 16;
    s->size = 0;
    s->threshold = 12;
    for (int32_t i = 0; i < s->cap; i++) {
        s->bucket[i] = NIL;
        s->tree[i] = 0;
    }
}

static int32_t root_of(const hc_jset_t *s, int32_t n) {
    while (ENT(s, n).parent != NIL)
        n = ENT(s, n).parent;
    return n;
}

/* TreeNode.moveRootToFront — root 를 체인 머리로 스플라이스 */
static void move_root_to_front(hc_jset_t *s, int32_t root) {
    if (root == NIL)
        return;
    int32_t index = (int32_t)(ENT(s, root).hash & (uint32_t)(s->cap - 1));
    int32_t first = s->bucket[index];
    if (root != first) {
        int32_t rn = ENT(s, root).next;
        int32_t rp = ENT(s, root).prev;
        s->bucket[index] = root;
        if (rn != NIL)
            ENT(s, rn).prev = rp;
        if (rp != NIL)
            ENT(s, rp).next = rn;
        if (first != NIL)
            ENT(s, first).prev = root;
        ENT(s, root).next = first;
        ENT(s, root).prev = NIL;
    }
}

/* TreeNode.rotateLeft */
static int32_t rotate_left(hc_jset_t *s, int32_t root, int32_t p) {
    int32_t r, pp, rl;
    if (p != NIL && (r = ENT(s, p).right) != NIL) {
        if ((rl = ENT(s, p).right = ENT(s, r).left) != NIL)
            ENT(s, rl).parent = p;
        if ((pp = ENT(s, r).parent = ENT(s, p).parent) == NIL) {
            root = r;
            ENT(s, r).red = 0;
        } else if (ENT(s, pp).left == p)
            ENT(s, pp).left = r;
        else
            ENT(s, pp).right = r;
        ENT(s, r).left = p;
        ENT(s, p).parent = r;
    }
    return root;
}

/* TreeNode.rotateRight */
static int32_t rotate_right(hc_jset_t *s, int32_t root, int32_t p) {
    int32_t l, pp, lr;
    if (p != NIL && (l = ENT(s, p).left) != NIL) {
        if ((lr = ENT(s, p).left = ENT(s, l).right) != NIL)
            ENT(s, lr).parent = p;
        if ((pp = ENT(s, l).parent = ENT(s, p).parent) == NIL) {
            root = l;
            ENT(s, l).red = 0;
        } else if (ENT(s, pp).right == p)
            ENT(s, pp).right = l;
        else
            ENT(s, pp).left = l;
        ENT(s, l).right = p;
        ENT(s, p).parent = l;
    }
    return root;
}

/* TreeNode.balanceInsertion */
static int32_t balance_insertion(hc_jset_t *s, int32_t root, int32_t x) {
    ENT(s, x).red = 1;
    for (int32_t xp, xpp, xppl, xppr;;) {
        if ((xp = ENT(s, x).parent) == NIL) {
            ENT(s, x).red = 0;
            return x;
        } else if (!ENT(s, xp).red || (xpp = ENT(s, xp).parent) == NIL)
            return root;
        if (xp == (xppl = ENT(s, xpp).left)) {
            if ((xppr = ENT(s, xpp).right) != NIL && ENT(s, xppr).red) {
                ENT(s, xppr).red = 0;
                ENT(s, xp).red = 0;
                ENT(s, xpp).red = 1;
                x = xpp;
            } else {
                if (x == ENT(s, xp).right) {
                    root = rotate_left(s, root, x = xp);
                    xpp = (xp = ENT(s, x).parent) == NIL ? NIL
                                                         : ENT(s, xp).parent;
                }
                if (xp != NIL) {
                    ENT(s, xp).red = 0;
                    if (xpp != NIL) {
                        ENT(s, xpp).red = 1;
                        root = rotate_right(s, root, xpp);
                    }
                }
            }
        } else {
            if (xppl != NIL && ENT(s, xppl).red) {
                ENT(s, xppl).red = 0;
                ENT(s, xp).red = 0;
                ENT(s, xpp).red = 1;
                x = xpp;
            } else {
                if (x == ENT(s, xp).left) {
                    root = rotate_right(s, root, x = xp);
                    xpp = (xp = ENT(s, x).parent) == NIL ? NIL
                                                         : ENT(s, xp).parent;
                }
                if (xp != NIL) {
                    ENT(s, xp).red = 0;
                    if (xpp != NIL) {
                        ENT(s, xpp).red = 1;
                        root = rotate_left(s, root, xpp);
                    }
                }
            }
        }
    }
}

/* TreeNode.balanceDeletion */
static int32_t balance_deletion(hc_jset_t *s, int32_t root, int32_t x) {
    for (int32_t xp, xpl, xpr;;) {
        if (x == NIL || x == root)
            return root;
        else if ((xp = ENT(s, x).parent) == NIL) {
            ENT(s, x).red = 0;
            return x;
        } else if (ENT(s, x).red) {
            ENT(s, x).red = 0;
            return root;
        } else if ((xpl = ENT(s, xp).left) == x) {
            if ((xpr = ENT(s, xp).right) != NIL && ENT(s, xpr).red) {
                ENT(s, xpr).red = 0;
                ENT(s, xp).red = 1;
                root = rotate_left(s, root, xp);
                xpr = (xp = ENT(s, x).parent) == NIL ? NIL : ENT(s, xp).right;
            }
            if (xpr == NIL)
                x = xp;
            else {
                int32_t sl = ENT(s, xpr).left, sr = ENT(s, xpr).right;
                if ((sr == NIL || !ENT(s, sr).red) &&
                    (sl == NIL || !ENT(s, sl).red)) {
                    ENT(s, xpr).red = 1;
                    x = xp;
                } else {
                    if (sr == NIL || !ENT(s, sr).red) {
                        if (sl != NIL)
                            ENT(s, sl).red = 0;
                        ENT(s, xpr).red = 1;
                        root = rotate_right(s, root, xpr);
                        xpr = (xp = ENT(s, x).parent) == NIL
                                  ? NIL
                                  : ENT(s, xp).right;
                    }
                    if (xpr != NIL) {
                        ENT(s, xpr).red = xp == NIL ? 0 : ENT(s, xp).red;
                        if ((sr = ENT(s, xpr).right) != NIL)
                            ENT(s, sr).red = 0;
                    }
                    if (xp != NIL) {
                        ENT(s, xp).red = 0;
                        root = rotate_left(s, root, xp);
                    }
                    x = root;
                }
            }
        } else { /* symmetric */
            if (xpl != NIL && ENT(s, xpl).red) {
                ENT(s, xpl).red = 0;
                ENT(s, xp).red = 1;
                root = rotate_right(s, root, xp);
                xpl = (xp = ENT(s, x).parent) == NIL ? NIL : ENT(s, xp).left;
            }
            if (xpl == NIL)
                x = xp;
            else {
                int32_t sl = ENT(s, xpl).left, sr = ENT(s, xpl).right;
                if ((sl == NIL || !ENT(s, sl).red) &&
                    (sr == NIL || !ENT(s, sr).red)) {
                    ENT(s, xpl).red = 1;
                    x = xp;
                } else {
                    if (sl == NIL || !ENT(s, sl).red) {
                        if (sr != NIL)
                            ENT(s, sr).red = 0;
                        ENT(s, xpl).red = 1;
                        root = rotate_left(s, root, xpl);
                        xpl = (xp = ENT(s, x).parent) == NIL
                                  ? NIL
                                  : ENT(s, xp).left;
                    }
                    if (xpl != NIL) {
                        ENT(s, xpl).red = xp == NIL ? 0 : ENT(s, xp).red;
                        if ((sl = ENT(s, xpl).left) != NIL)
                            ENT(s, sl).red = 0;
                    }
                    if (xp != NIL) {
                        ENT(s, xp).red = 0;
                        root = rotate_right(s, root, xp);
                    }
                    x = root;
                }
            }
        }
    }
}

/* TreeNode.treeify — 체인(next 순서)에서 RB 트리 구축 + moveRootToFront.
 * 호출 전 체인의 prev 링크가 유효해야 한다. */
static void treeify(hc_jset_t *s, int32_t idx) {
    int32_t root = NIL;
    int32_t nxt;
    for (int32_t x = s->bucket[idx]; x != NIL; x = nxt) {
        nxt = ENT(s, x).next;
        ENT(s, x).left = ENT(s, x).right = NIL;
        if (root == NIL) {
            ENT(s, x).parent = NIL;
            ENT(s, x).red = 0;
            root = x;
        } else {
            int32_t h = (int32_t)ENT(s, x).hash;
            for (int32_t p = root;;) {
                int32_t dir;
                int32_t ph = (int32_t)ENT(s, p).hash;
                if (ph > h)
                    dir = -1;
                else if (ph < h)
                    dir = 1;
                else
                    die("jset treeify equal-hash tie-break "
                        "(identityHashCode) reached",
                        NULL);
                int32_t xp = p;
                p = dir <= 0 ? ENT(s, p).left : ENT(s, p).right;
                if (p == NIL) {
                    ENT(s, x).parent = xp;
                    if (dir <= 0)
                        ENT(s, xp).left = x;
                    else
                        ENT(s, xp).right = x;
                    root = balance_insertion(s, root, x);
                    break;
                }
            }
        }
    }
    s->tree[idx] = 1;
    move_root_to_front(s, root);
}

/* resize 내 TreeNode.split — lo/hi 로 분할 (prev 유지), 반쪽이
 * UNTREEIFY_THRESHOLD(6) 이하이면 untreeify(체인 순서 유지), 아니면
 * 상대 반쪽이 있을 때만 재트리화 (없으면 트리 구조 그대로). */
static void split_tree(hc_jset_t *s, int32_t j, int32_t bit) {
    int32_t lo_head = NIL, lo_tail = NIL, hi_head = NIL, hi_tail = NIL;
    int32_t lc = 0, hc = 0;
    int32_t nxt;
    for (int32_t e = s->bucket[j]; e != NIL; e = nxt) {
        nxt = ENT(s, e).next;
        ENT(s, e).next = NIL;
        if ((ENT(s, e).hash & (uint32_t)bit) == 0) {
            ENT(s, e).prev = lo_tail;
            if (lo_tail == NIL)
                lo_head = e;
            else
                ENT(s, lo_tail).next = e;
            lo_tail = e;
            lc++;
        } else {
            ENT(s, e).prev = hi_tail;
            if (hi_tail == NIL)
                hi_head = e;
            else
                ENT(s, hi_tail).next = e;
            hi_tail = e;
            hc++;
        }
    }
    s->bucket[j] = NIL;
    s->tree[j] = 0;
    s->bucket[j + bit] = NIL;
    s->tree[j + bit] = 0;
    if (lo_head != NIL) {
        s->bucket[j] = lo_head;
        if (lc <= 6) {
            /* untreeify — 체인 순서 그대로, 트리 필드 무시 */
        } else {
            s->tree[j] = 1;
            if (hi_head != NIL)
                treeify(s, j);
            /* else: 트리 구조 그대로 (전 노드 잔류) */
        }
    }
    if (hi_head != NIL) {
        s->bucket[j + bit] = hi_head;
        if (hc <= 6) {
            /* untreeify */
        } else {
            s->tree[j + bit] = 1;
            if (lo_head != NIL)
                treeify(s, j + bit);
        }
    }
}

/* HashMap.resize — 버킷을 lo/hi 로 상대순서 보존 분할 */
static void jset_resize(hc_jset_t *s) {
    int32_t old_cap = s->cap;
    if (old_cap * 2 > HC_JSET_MAX_CAP)
        die("jset resize overflow", NULL);
    s->cap = old_cap * 2;
    s->threshold = (int32_t)((double)s->cap * 0.75);
    for (int32_t j = 0; j < old_cap; j++) {
        int32_t head = s->bucket[j];
        s->bucket[j + old_cap] = NIL;
        s->tree[j + old_cap] = 0;
        if (head == NIL)
            continue;
        if (s->tree[j]) {
            split_tree(s, j, old_cap);
            continue;
        }
        int32_t lo_head = NIL, lo_tail = NIL, hi_head = NIL, hi_tail = NIL;
        int32_t e = head;
        while (e != NIL) {
            int32_t nxt = ENT(s, e).next;
            ENT(s, e).next = NIL;
            if ((ENT(s, e).hash & (uint32_t)old_cap) == 0) {
                if (lo_tail == NIL)
                    lo_head = e;
                else
                    ENT(s, lo_tail).next = e;
                lo_tail = e;
            } else {
                if (hi_tail == NIL)
                    hi_head = e;
                else
                    ENT(s, hi_tail).next = e;
                hi_tail = e;
            }
            e = nxt;
        }
        s->bucket[j] = lo_head;
        s->bucket[j + old_cap] = hi_head;
    }
}

/* treeifyBin: cap < MIN_TREEIFY_CAPACITY(64) → resize 1회, 아니면 체인의
 * prev 링크를 세우고 treeify */
static void treeify_bin(hc_jset_t *s, int32_t idx) {
    if (s->cap < 64) {
        jset_resize(s);
        return;
    }
    int32_t head = s->bucket[idx];
    if (head == NIL)
        return;
    int32_t prev = NIL;
    for (int32_t e = head; e != NIL; e = ENT(s, e).next) {
        ENT(s, e).prev = prev;
        prev = e;
    }
    treeify(s, idx);
}

static int32_t new_entry(hc_jset_t *s, int32_t x, int32_t y, int32_t z,
                         uint32_t h) {
    if (s->n_ent >= HC_JSET_MAX_ENTRIES)
        die("jset entries overflow", NULL);
    int32_t ne = s->n_ent++;
    ENT(s, ne).x = x;
    ENT(s, ne).y = y;
    ENT(s, ne).z = z;
    ENT(s, ne).hash = h;
    ENT(s, ne).next = NIL;
    ENT(s, ne).prev = NIL;
    ENT(s, ne).parent = NIL;
    ENT(s, ne).left = NIL;
    ENT(s, ne).right = NIL;
    ENT(s, ne).red = 0;
    ENT(s, ne).dead = 0;
    return ne;
}

/* TreeNode.putTreeVal — 트리 빈 삽입: 새 노드는 트리 부모(xp) 뒤에
 * 체인 링크, balanceInsertion + moveRootToFront. 반환: 삽입 1 / 존재 0 */
static int put_tree_val(hc_jset_t *s, int32_t idx, int32_t x, int32_t y,
                        int32_t z, uint32_t h) {
    int32_t first = s->bucket[idx];
    int32_t root =
        ENT(s, first).parent != NIL ? root_of(s, first) : first;
    for (int32_t p = root;;) {
        int32_t dir;
        int32_t ph = (int32_t)ENT(s, p).hash;
        if (ph > (int32_t)h)
            dir = -1;
        else if (ph < (int32_t)h)
            dir = 1;
        else if (ENT(s, p).x == x && ENT(s, p).y == y && ENT(s, p).z == z)
            return 0; /* found */
        else
            die("jset putTreeVal equal-hash tie-break (identityHashCode) "
                "reached",
                NULL);
        int32_t xp = p;
        p = dir <= 0 ? ENT(s, p).left : ENT(s, p).right;
        if (p == NIL) {
            int32_t xpn = ENT(s, xp).next;
            int32_t ne = new_entry(s, x, y, z, h);
            ENT(s, ne).next = xpn;
            if (dir <= 0)
                ENT(s, xp).left = ne;
            else
                ENT(s, xp).right = ne;
            ENT(s, xp).next = ne;
            ENT(s, ne).parent = xp;
            ENT(s, ne).prev = xp;
            if (xpn != NIL)
                ENT(s, xpn).prev = ne;
            move_root_to_front(s, balance_insertion(s, root, ne));
            return 1;
        }
    }
}

int hc_jset_add(hc_jset_t *s, int32_t x, int32_t y, int32_t z) {
    uint32_t h = spread_hash(x, y, z);
    int32_t  idx = (int32_t)(h & (uint32_t)(s->cap - 1));
    int32_t  head = s->bucket[idx];
    if (head == NIL) {
        s->bucket[idx] = new_entry(s, x, y, z, h);
        s->tree[idx] = 0;
    } else if (s->tree[idx]) {
        if (!put_tree_val(s, idx, x, y, z, h))
            return 0;
    } else {
        int32_t e = head, tail = NIL, bin = 0;
        while (e != NIL) {
            if (ENT(s, e).x == x && ENT(s, e).y == y && ENT(s, e).z == z)
                return 0;
            tail = e;
            bin++;
            e = ENT(s, e).next;
        }
        int32_t ne = new_entry(s, x, y, z, h);
        ENT(s, tail).next = ne;
        /* putVal: binCount(첫 노드 제외) >= TREEIFY_THRESHOLD(8)-1 —
         * 체인이 9개가 되는 삽입에서 treeifyBin */
        if (bin >= 8)
            treeify_bin(s, idx);
    }
    if (++s->size > s->threshold)
        jset_resize(s);
    return 1;
}

/* TreeNode.removeTreeNode(map, tab, movable) — nd 를 트리 빈에서 제거.
 * JDK 25 검증: too-small untreeify 는 movable 게이트 (iterator remove =
 * movable false 는 트리를 유지한다). */
static void remove_tree_node(hc_jset_t *s, int32_t nd, int movable) {
    int32_t index = (int32_t)(ENT(s, nd).hash & (uint32_t)(s->cap - 1));
    int32_t first = s->bucket[index];
    int32_t root = first;
    int32_t succ = ENT(s, nd).next, pred = ENT(s, nd).prev;
    if (pred == NIL) {
        s->bucket[index] = succ;
        first = succ;
    } else
        ENT(s, pred).next = succ;
    if (succ != NIL)
        ENT(s, succ).prev = pred;
    if (first == NIL) {
        s->tree[index] = 0; /* 빈 버킷 — 다음 삽입은 평범한 노드 */
        return;
    }
    if (ENT(s, root).parent != NIL)
        root = root_of(s, root);
    int32_t rl;
    if (root == NIL ||
        (movable &&
         (ENT(s, root).right == NIL || (rl = ENT(s, root).left) == NIL ||
          ENT(s, rl).left == NIL))) {
        s->tree[index] = 0; /* untreeify — 체인 순서 그대로 */
        return;
    }
    int32_t p = nd, pl = ENT(s, nd).left, pr = ENT(s, nd).right, replacement;
    if (pl != NIL && pr != NIL) {
        int32_t sn = pr, sl;
        while ((sl = ENT(s, sn).left) != NIL)
            sn = sl;
        {
            uint8_t c = ENT(s, sn).red;
            ENT(s, sn).red = ENT(s, p).red;
            ENT(s, p).red = c;
        }
        int32_t sr = ENT(s, sn).right;
        int32_t pp = ENT(s, p).parent;
        if (sn == pr) {
            ENT(s, p).parent = sn;
            ENT(s, sn).right = p;
        } else {
            int32_t sp = ENT(s, sn).parent;
            if ((ENT(s, p).parent = sp) != NIL) {
                if (sn == ENT(s, sp).left)
                    ENT(s, sp).left = p;
                else
                    ENT(s, sp).right = p;
            }
            if ((ENT(s, sn).right = pr) != NIL)
                ENT(s, pr).parent = sn;
        }
        ENT(s, p).left = NIL;
        if ((ENT(s, p).right = sr) != NIL)
            ENT(s, sr).parent = p;
        if ((ENT(s, sn).left = pl) != NIL)
            ENT(s, pl).parent = sn;
        if ((ENT(s, sn).parent = pp) == NIL)
            root = sn;
        else if (p == ENT(s, pp).left)
            ENT(s, pp).left = sn;
        else
            ENT(s, pp).right = sn;
        if (sr != NIL)
            replacement = sr;
        else
            replacement = p;
    } else if (pl != NIL)
        replacement = pl;
    else if (pr != NIL)
        replacement = pr;
    else
        replacement = p;
    if (replacement != p) {
        int32_t pp = ENT(s, replacement).parent = ENT(s, p).parent;
        if (pp == NIL) {
            root = replacement;
            ENT(s, replacement).red = 0;
        } else if (p == ENT(s, pp).left)
            ENT(s, pp).left = replacement;
        else
            ENT(s, pp).right = replacement;
        ENT(s, p).left = ENT(s, p).right = ENT(s, p).parent = NIL;
    }
    int32_t r = ENT(s, p).red ? root : balance_deletion(s, root, replacement);
    if (replacement == p) { /* detach */
        int32_t pp = ENT(s, p).parent;
        ENT(s, p).parent = NIL;
        if (pp != NIL) {
            if (p == ENT(s, pp).left)
                ENT(s, pp).left = NIL;
            else if (p == ENT(s, pp).right)
                ENT(s, pp).right = NIL;
        }
    }
    if (movable)
        move_root_to_front(s, r);
}

int hc_jset_poll_first(hc_jset_t *s, int32_t *x, int32_t *y, int32_t *z) {
    for (int32_t b = 0; b < s->cap; b++) {
        int32_t e = s->bucket[b];
        if (e == NIL)
            continue;
        *x = ENT(s, e).x;
        *y = ENT(s, e).y;
        *z = ENT(s, e).z;
        if (s->tree[b]) {
            /* HashIterator.remove → removeNode(..., movable=false) */
            remove_tree_node(s, e, 0);
        } else {
            s->bucket[b] = ENT(s, e).next;
        }
        ENT(s, e).dead = 1;
        s->size--;
        return 1;
    }
    return 0;
}

void hc_jit_begin(hc_jit_t *it, const hc_jset_t *s) {
    it->s = s;
    it->b = -1;
    it->e = NIL;
    for (int32_t b = 0; b < s->cap; b++)
        if (s->bucket[b] != NIL) {
            it->b = b;
            it->e = s->bucket[b];
            return;
        }
    it->b = s->cap;
}

int hc_jit_valid(const hc_jit_t *it) {
    return it->b < it->s->cap && it->e != NIL;
}

void hc_jit_next(hc_jit_t *it) {
    const hc_jset_t *s = it->s;
    if (ENT(s, it->e).next != NIL) {
        it->e = ENT(s, it->e).next;
        return;
    }
    for (int32_t b = it->b + 1; b < s->cap; b++)
        if (s->bucket[b] != NIL) {
            it->b = b;
            it->e = s->bucket[b];
            return;
        }
    it->b = s->cap;
    it->e = NIL;
}

/* Task 14: 순회 위치의 좌표 접근자 (structures.c BE 순서 재구성용) */
int hc_jit_pos(const hc_jit_t *it, int32_t *x, int32_t *y, int32_t *z) {
    if (!hc_jit_valid(it))
        return 0;
    const hc_jent_t *e = &it->s->ent[it->e];
    *x = e->x;
    *y = e->y;
    *z = e->z;
    return 1;
}
