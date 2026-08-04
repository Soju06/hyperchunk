# R-fastutil — fastutil 8.5.18 해시 컨테이너 순회 순서 시맨틱 핀 (C 포팅용)

조사 기준: MC 26.2 서버 번들 **fastutil-8.5.18**
(`tools/golden/libs/extracted/libraries/it/unimi/dsi/fastutil/8.5.18/fastutil-8.5.18.jar`).
vineflower 1.11.1 디컴파일 → `tools/golden/work/task14-fastutil-decomp/it/unimi/dsi/fastutil/`
(gitignored 로컬; 아래 인용 라인번호는 이 출력 기준).
검증 에뮬레이터(Python): `tools/golden/work/task14-fastutil-decomp/emulate_longopenhashset.py`.

핀 대상 클래스: `HashCommon`, `longs/LongOpenHashSet`, `objects/Object2ObjectOpenHashMap`
(+ 방출 경로 확인용 `longs/AbstractLongCollection`, `longs/LongIterators`).

---

## 1. HashCommon.mix — 산식 (HashCommon.java L4–7, L28–47)

```java
private static final int  INT_PHI  = -1640531527;            // 0x9E3779B9
private static final long LONG_PHI = -7046029254386353131L;  // 0x9E3779B97F4A7C15

public static int mix(int x)  { int h = x * -1640531527; return h ^ h >>> 16; }
public static long mix(long x){ long h = x * -7046029254386353131L; h ^= h >>> 32; return h ^ h >>> 16; }
```

C 포팅 (wrap 곱셈 + 논리 시프트):

```c
static inline uint32_t hc_mix32(uint32_t x) {          /* mix(int) */
    uint32_t h = x * 0x9E3779B9u;
    return h ^ (h >> 16);
}
static inline uint64_t hc_mix64(uint64_t x) {          /* mix(long) */
    uint64_t h = x * 0x9E3779B97F4A7C15ull;
    h ^= h >> 32;
    return h ^ (h >> 16);
}
```

슬롯 결정에서 `(int)mix(long)` 캐스트는 하위 32비트 취득이지만, `mask < 2^31` 이므로
`(int)mix(k) & mask == (int)(mix64(k) & mask)` — C 에선 `hc_mix64(k) & (uint64_t)mask` 로 충분.

## 2. 공통 사이징 함수 (HashCommon.java L62–85)

```java
public static int nextPowerOfTwo(int x) { return 1 << 32 - Integer.numberOfLeadingZeros(x - 1); }
public static long nextPowerOfTwo(long x){ return 1L << 64 - Long.numberOfLeadingZeros(x - 1L); }
public static int maxFill(int n, float f){ return Math.min((int)Math.ceil((double)n * f), n - 1); }
public static int arraySize(int expected, float f) {
    long s = Math.max(2L, nextPowerOfTwo((long)Math.ceil((double)expected / f)));
    if (s > 1073741824L) throw ...; return (int)s;
}
```

- `nextPowerOfTwo(x)` = x≥1 에서 `1 << bit_length(x-1)` (x=1→1, x=0→1: Java 시프트 64 wrap).
- `f` 는 float `0.75f` — 이진 정확값이라 double 승격 후 ceil 에 반올림 이슈 없음. C: `ceil((double)expected / 0.75)`.
- 기본 케이스: `arraySize(16, 0.75)` = nextPow2(ceil(21.33)=22) = **32**; `maxFill(32, 0.75)` = min(24, 31) = **24**.

## 3. LongOpenHashSet

필드 (L20–27): `long[] key` (길이 **n+1**; 인덱스 n 은 null(0) 키 전용 슬롯), `mask = n-1`,
`boolean containsNull`, `int n, maxFill, size`, `minN = 초기 n`.

### 3.1 생성자

- 기본 생성자 (L47–49): `this(16, 0.75F)` → **n=32, mask=31, maxFill=24, key.length=33** (L29–41).
- `new LongOpenHashSet(long[] a)` (L106–108 → L102 → L89–96): `expected = a.length`, f=0.75 →
  **초기 n = arraySize(a.length, 0.75) = max(2, nextPow2(ceil(a.length/0.75)))**,
  그 후 `a[0..len)` 을 **배열 순서대로 add**. 이 채움 중 rehash 는 불가능
  (maxFill(arraySize(e,f),f) ≥ e; 중복은 add 가 false 반환하고 size 미증가).

### 3.2 add(long k) — 슬롯 결정 (L206–237)

```java
public boolean add(long k) {
    if (k == 0L) {
        if (this.containsNull) return false;
        this.containsNull = true;                     // key[n]은 0 그대로 (쓰지 않음)
    } else {
        if ((curr = key[pos = (int)HashCommon.mix(k) & this.mask]) != 0L) {
            if (curr == k) return false;
            while ((curr = key[pos = pos + 1 & this.mask]) != 0L)   // ★ 전방(+1) 선형 프로브
                if (curr == k) return false;
        }
        key[pos] = k;
    }
    if (this.size++ >= this.maxFill)                  // ★ post-increment: 옛 size 비교
        this.rehash(HashCommon.arraySize(this.size + 1, this.f));   // 이 시점 size는 이미 +1
    return true;
}
```

- 슬롯: `pos = (int)mix(k) & mask`; 충돌 시 **pos = (pos+1) & mask 전방 순회** (역방향 아님).
- 0 키: 테이블 밖 플래그 `containsNull` (개념상 슬롯 n). 중복 0 은 size/rehash 체크도 건너뜀.
- rehash 조건: 삽입 **후** `oldSize >= maxFill` 이면 `rehash(arraySize(oldSize + 2, f))`
  (post-increment 라 인자 안 `this.size + 1 == oldSize + 2`).
  기본 세트에선 25번째 신규 원소에서 24≥24 → `rehash(arraySize(26,0.75)=64)`.
  **새 원소는 옛 테이블에 먼저 들어간 뒤** rehash 가 전체를 재배치.

### 3.3 rehash(int newN) — 재배치 순서 (L384–408)

```java
int i = this.n;  int j = this.realSize();            // realSize = size - (containsNull?1:0)
while (j-- != 0) {
    while (key[--i] == 0L) {}                        // ★ 옛 배열을 n-1 → 0 내림차순 스캔
    if (newKey[pos = (int)HashCommon.mix(key[i]) & mask] != 0L)
        while (newKey[pos = pos + 1 & mask] != 0L) {}
    newKey[pos] = key[i];
}
```

**옛 배열의 높은 인덱스 → 낮은 인덱스** 순으로 꺼내서 새 테이블에 (mix & newMask, 전방 프로브) 재삽입.
containsNull/0 키는 재배치 없음 (플래그 유지, newKey[newN]=0).

### 3.4 방출 순서 — iterator / toLongArray

`toLongArray()` (AbstractLongCollection.java L71–80): `new long[size]` 에
`LongIterators.unwrap(iterator(), a)` (LongIterators.java L48–62 — 앞에서부터 순서대로 채움)
→ **SetIterator 순서 그대로**.

`SetIterator` (LongOpenHashSet.java L476–511):

```java
int pos = LongOpenHashSet.this.n;
boolean mustReturnNull = LongOpenHashSet.this.containsNull;
public long nextLong() {
    ...
    if (this.mustReturnNull) {                       // ★ 0(null 키)이 있으면 가장 먼저 방출
        this.mustReturnNull = false; this.last = n; return key[n];   // == 0
    }
    while (--this.pos >= 0)                          // ★ n-1 → 0 내림차순 스캔
        if (key[this.pos] != 0L) return key[this.last = this.pos];
    ... this.wrapped.getLong(...)                    // iteration 중 remove 전용 — add-only 에선 도달 불가
}
```

**방출 순서 = (0 이 원소면 0 먼저) + 배열 인덱스 n-1 → 0 내림차순의 비-0 원소.**
`forEach` (L351–364) 도 동일 순서. `wrapped` 경로는 순회 중 `remove()` 가 시프트로
커서 뒤로 원소를 옮길 때만 — add 후 방출만 하는 references 경로에선 절대 안 걸림.

### 3.5 C 의사코드 (전체)

```c
typedef struct { uint64_t *key; int n, mask, maxFill, size; bool containsNull; } LOHS;
/* init(expected=16): n=arraySize(expected,0.75); mask=n-1; maxFill=min((int)ceil(n*0.75),n-1);
   key=calloc(n+1, 8); */
bool lohs_add(LOHS *s, uint64_t k) {
    if (k == 0) { if (s->containsNull) return false; s->containsNull = true; }
    else {
        int pos = (int)(hc_mix64(k) & (uint64_t)s->mask);
        for (uint64_t curr; (curr = s->key[pos]) != 0; pos = (pos + 1) & s->mask)
            if (curr == k) return false;
        s->key[pos] = k;
    }
    if (s->size++ >= s->maxFill) lohs_rehash(s, arraySize(s->size + 1, 0.75)); /* size는 이미 +1 */
    return true;
}
void lohs_rehash(LOHS *s, int newN) {
    uint64_t *nk = calloc(newN + 1, 8); int mask = newN - 1;
    int i = s->n, j = s->size - (s->containsNull ? 1 : 0);
    while (j-- != 0) {
        while (s->key[--i] == 0) {}
        int pos = (int)(hc_mix64(s->key[i]) & (uint64_t)mask);
        while (nk[pos] != 0) pos = (pos + 1) & mask;
        nk[pos] = s->key[i];
    }
    free(s->key); s->key = nk; s->n = newN; s->mask = mask;
    s->maxFill = maxFill(newN, 0.75);
}
int lohs_to_array(const LOHS *s, uint64_t *out) {          /* == toLongArray 방출 순서 */
    int c = 0;
    if (s->containsNull) out[c++] = 0;
    for (int pos = s->n - 1; pos >= 0; pos--)
        if (s->key[pos] != 0) out[c++] = s->key[pos];
    return c;                                              /* == size */
}
```

## 4. Object2ObjectOpenHashMap — 동일 골격, int mix

필드/생성자 (L20–57): `K[] key, V[] value` 길이 n+1, 기본 생성자 `this(16, 0.75F)` → n=32.
빈 슬롯 센티널은 **null 참조** (long 세트의 0 에 대응).

### 4.1 put — 슬롯 결정 (find L145–166, insert L168–178, put L180–190)

```java
private int find(K k) {
    if (k == null) return this.containsNullKey ? this.n : -(this.n + 1);   // null 키 → 슬롯 n
    if ((curr = key[pos = HashCommon.mix(k.hashCode()) & this.mask]) == null) return -(pos + 1);
    if (k.equals(curr)) return pos;
    while ((curr = key[pos = pos + 1 & this.mask]) != null)                // ★ 전방 프로브
        if (k.equals(curr)) return pos;
    return -(pos + 1);
}
private void insert(int pos, K k, V v) {
    if (pos == this.n) this.containsNullKey = true;
    this.key[pos] = k; this.value[pos] = v;
    if (this.size++ >= this.maxFill) this.rehash(HashCommon.arraySize(this.size + 1, this.f));
}
```

- 슬롯: **`mix(int)` 를 `k.hashCode()` 에 적용** — `h = hash * 0x9E3779B9` (32비트 wrap) `; h ^ h>>>16`,
  그 뒤 `& mask`. 등가 판정은 `k.equals(curr)` (프로브는 null 슬롯에서 정지).
- put = find 로 전체 프로브 후, 부재 시 프로브가 멈춘 첫 빈 슬롯에 insert. 기존 키면 value 만 교체 (재배치 없음).
- rehash 조건/인자: LongOpenHashSet 3.2 와 동일 (post-increment 포함).
- 주의: `k.hashCode()` 자체(String.hashCode 등)는 대상 키 타입별로 별도 핀 필요 — 이 노트 범위 밖.

### 4.2 rehash (L584–614)

LongOpenHashSet 3.3 과 동일 구조: 옛 배열 **n-1 → 0 내림차순** 스캔, `mix32(hashCode) & newMask` + 전방 프로브,
`newValue[pos] = value[i]` 동반 이동. 마지막에 `newValue[newN] = value[this.n]` — **null 키의 값 보존** (키는 플래그).

### 4.3 순회 순서 — keySet() / values() (MapIterator L1051–1095, KeyIterator L774, ValueIterator L1310, values() L522)

`KeyIterator`/`ValueIterator`/`EntryIterator` 모두 공통 `MapIterator.nextEntry()` 사용:

```java
int pos = Object2ObjectOpenHashMap.this.n;
boolean mustReturnNullKey = Object2ObjectOpenHashMap.this.containsNullKey;
public int nextEntry() {
    ...
    if (this.mustReturnNullKey) { this.mustReturnNullKey = false; return this.last = n; }  // null 키 먼저
    while (--this.pos >= 0)
        if (key[this.pos] != null) return this.last = this.pos;                            // n-1 → 0
    ... wrapped ...                                    // 순회 중 remove 전용
}
```

**keySet().iterator() 와 values().iterator() 는 같은 엔트리 순서**:
(null 키 엔트리 먼저) + 배열 인덱스 **n-1 → 0 내림차순**. `values()` 의 `forEach` (L533–543) 도 동일
(`containsNullKey` 먼저 → `pos--` 내림차순). LongOpenHashSet 3.4 와 완전히 같은 규칙, 센티널만 null.

---

## 5. 검증 — 골든 `golden/structures/references.txt` (r.0.0 실측)

**셋업**: 각 (cx,cz,structure) 그룹의 골든 라인 순서 = 디스크 LongArray 방출 순서.
삽입 순서 = 소스 청크 스캔 순서 (sourceX 외측 −8..+8 오름차순, sourceZ 내측 오름차순)
= 멤버 (sx,sz) 사전식 오름차순. **멤버 집합은 골든 자체에서 취득** — vanilla `createReferences` 는
반경 8 스캔에 더해 **start bbox ∩ 대상 청크 16×16 교차 필터**가 걸려 있어 반경만으로는 과포함
(실측: (25,0) 은 x거리 8 인 (33,−1),(33,−2) 를 참조하지 않고, (28,0) 은 (34,4)/(20,−4) 를 참조하지 않음
— 순수 반경-8 멤버십 가정은 151 그룹 중 19 그룹 불일치). C 쪽 references 생성은 bbox 필터까지
구현해야 함 (멤버십은 placement 노트 소관; 이 노트는 **순서** 핀).

**결과** (`emulate_longopenhashset.py`, 기본 생성자 LongOpenHashSet + add + toLongArray 순서):

| structure | 그룹 일치 |
|---|---|
| minecraft:mineshaft | **39/39** |
| minecraft:trial_chambers | **108/108** |
| minecraft:ocean_ruin_warm | 2/2 |
| minecraft:ruined_portal_ocean | 1/1 |
| minecraft:shipwreck_beached | 1/1 |
| **합계** | **151/151** (레퍼런스 라인 177개 전부) |

다중 참조(≥2개) 그룹 18개 — 순서 판별력 있는 케이스 — 전부 일치.

**예시** (30,0) mineshaft: 삽입 (33,−2),(33,−1),(34,4) → mix64 슬롯 (mask 31): 2, 4, 16 →
내림차순 방출 = `0x0000000400000022` (34,4), `0xffffffff00000021` (33,−1), `0xfffffffe00000021` (33,−2)
— 골든과 일치. 슬롯표: (20,−4)→23, (33,−2)→2, (33,−1)→4, (34,4)→16.

**네거티브 컨트롤** (판별력 증명):
- iterator 를 오름차순(0→n-1)으로 바꾸면 **133/151** (다중 그룹 18개 전멸) → 내림차순 방향은 골든이 핀.
- mix64 대신 murmurHash3 finalizer 를 쓰면 **141/151** → PHI mix 상수는 골든이 핀.
- 삽입 순서를 뒤집어도 **151/151 통과** — 이 골든에선 그룹 내 슬롯 충돌 0건, 최대 3원소로 rehash 0건이라
  방출이 순수 슬롯 결정. **커버리지 경계: 전방 프로브 방향·rehash 트리거/재배치 순서·삽입 순서 의존성은
  이 골든이 블라인드** — 디컴파일 소스(3.2/3.3 인용)로만 핀. 충돌·리사이즈가 걸리는 골든(예: 대형 세트
  직렬화 경로)을 확보하면 재검증할 것.

## 6. C 포팅 체크리스트

1. 곱셈은 uint32/uint64 wrap, 시프트는 논리(`>>>`) — signed 로 하지 말 것.
2. 슬롯 = `hc_mix64(k) & mask` (세트) / `hc_mix32((uint32_t)hashCode) & mask` (맵). 프로브 `(pos+1)&mask` 전방.
3. rehash 트리거는 post-increment 의미까지: 새 원소 삽입 → `oldSize >= maxFill` 검사 → `arraySize(oldSize+2, f)`.
4. rehash 재배치는 옛 배열 **내림차순** 스캔 (충돌 시 새 테이블 내 상대 순서가 여기서 결정됨).
5. 방출(직렬화) = null/0 먼저 + 내림차순 스캔. `toLongArray` 는 iterator 를 그대로 unwrap.
6. `ceil` 은 double 로 (f=0.75 는 정확값이라 안전).
