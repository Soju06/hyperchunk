import java.io.PrintWriter;
import java.lang.reflect.Method;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.List;
import java.util.Optional;

import net.minecraft.core.BlockPos;
import net.minecraft.core.Direction;
import net.minecraft.core.registries.BuiltInRegistries;
import net.minecraft.resources.Identifier;
import net.minecraft.world.level.EmptyBlockGetter;
import net.minecraft.world.level.block.Block;
import net.minecraft.world.level.block.state.BlockState;
import net.minecraft.world.level.block.state.properties.Property;
import net.minecraft.world.phys.AABB;
import net.minecraft.world.phys.shapes.VoxelShape;

/**
 * Task 14: R-blockprops TSV 행을 실서버에서 직접 측정해 덤프한다.
 * 입력: 상태 문자열 목록 파일 ("minecraft:base[k=v,...]" 캐노니컬,
 * 한 줄 하나). 출력: R-blockprops TSV 스키마 (state, blocksMotion,
 * legacySolid, fullOcclude, replaceable, emission, lightBlock,
 * shapeOcclusion, notes, collisionFull).
 *
 * shapeOcclusion 니블 규약 = R-blockprops-evidence §4 (면당 2x2 쿼드런트,
 * world-axis, all-or-nothing — 부분 커버 쿼드런트가 나오면 실패).
 */
public final class StatePropsProbe {
    public static void main(String[] args) throws Exception {
        net.minecraft.SharedConstants.tryDetectVersion();
        net.minecraft.server.Bootstrap.bootStrap();

        List<String> wanted = Files.readAllLines(Path.of(args[0]));
        Path out = Path.of(args[1]);
        try (PrintWriter w = new PrintWriter(Files.newBufferedWriter(out))) {
            w.println("state\tblocksMotion\tlegacySolid\tfullOcclude\t"
                      + "replaceable\temission\tlightBlock\tshapeOcclusion\t"
                      + "notes\tcollisionFull");
            for (String line : wanted) {
                line = line.trim();
                if (line.isEmpty() || line.startsWith("#"))
                    continue;
                BlockState st = parse(line);
                String canon = canonical(st);
                if (!canon.equals(line))
                    throw new IllegalStateException(
                        "canonical mismatch: " + line + " vs " + canon);
                w.printf("%s\t%s\t%s\t%s\t%s\t%d\t%d\t%s\tprobe\t%s%n",
                         canon, tf(st.blocksMotion()), tf(st.isSolid()),
                         tf(st.isSolidRender()), tf(st.canBeReplaced()),
                         st.getLightEmission(), lightBlock(st),
                         occlusion(st),
                         tf(st.isCollisionShapeFullBlock(
                             EmptyBlockGetter.INSTANCE, BlockPos.ZERO)));
            }
        }
        System.out.println("wrote " + out);
    }

    static String tf(boolean b) {
        return b ? "t" : "f";
    }

    static int lightBlock(BlockState st) throws Exception {
        for (String m : new String[] {"getLightBlock", "getLightDampening"}) {
            try {
                Method mm = st.getClass().getMethod(m);
                return (Integer) mm.invoke(st);
            } catch (NoSuchMethodException e) {
                // try next
            }
        }
        java.lang.reflect.Field f = null;
        for (Class<?> c = st.getClass(); c != null; c = c.getSuperclass()) {
            try {
                f = c.getDeclaredField("lightDampening");
                break;
            } catch (NoSuchFieldException e) {
                // ascend
            }
        }
        f.setAccessible(true);
        return f.getInt(st);
    }

    @SuppressWarnings({"unchecked", "rawtypes"})
    static BlockState parse(String s) {
        String base = s, props = null;
        int br = s.indexOf('[');
        if (br >= 0) {
            base = s.substring(0, br);
            props = s.substring(br + 1, s.length() - 1);
        }
        Block b = BuiltInRegistries.BLOCK.getValue(Identifier.parse(base));
        if (b == null || (!base.equals("minecraft:air")
                          && BuiltInRegistries.BLOCK.getKey(b).toString()
                                 .equals("minecraft:air")))
            throw new IllegalArgumentException("unknown block " + base);
        BlockState st = b.defaultBlockState();
        if (props != null && !props.isEmpty())
            for (String kv : props.split(",")) {
                String[] p2 = kv.split("=", 2);
                Property prop = b.getStateDefinition().getProperty(p2[0]);
                if (prop == null)
                    throw new IllegalArgumentException("no prop " + kv);
                Optional<?> v = prop.getValue(p2[1]);
                st = st.setValue(prop, (Comparable) v.orElseThrow());
            }
        return st;
    }

    static String canonical(BlockState st) {
        StringBuilder sb = new StringBuilder(
            BuiltInRegistries.BLOCK.getKey(st.getBlock()).toString());
        var props = st.getProperties().stream()
                        .sorted((a, b2) -> a.getName().compareTo(b2.getName()))
                        .toList();
        if (!props.isEmpty()) {
            sb.append('[');
            boolean first = true;
            for (Property<?> p : props) {
                if (!first)
                    sb.append(',');
                first = false;
                sb.append(p.getName()).append('=').append(nameOf(st, p));
            }
            sb.append(']');
        }
        return sb.toString();
    }

    static <T extends Comparable<T>> String nameOf(BlockState st,
                                                   Property<T> p) {
        return p.getName(st.getValue(p));
    }

    /* R-blockprops-evidence §4: useShapeForLightOcclusion 이 아니면 none;
     * 면당 슬라이스의 2x2 쿼드런트 all-or-nothing 니블. */
    static String occlusion(BlockState st) {
        if (!st.useShapeForLightOcclusion())
            return "none";
        StringBuilder sb = new StringBuilder();
        String[] tag = {"D", "U", "N", "S", "W", "E"};
        Direction[] dirs = {Direction.DOWN, Direction.UP, Direction.NORTH,
                            Direction.SOUTH, Direction.WEST, Direction.EAST};
        for (int i = 0; i < 6; i++) {
            VoxelShape face = st.getFaceOcclusionShape(dirs[i]);
            int nib = 0;
            for (int a = 0; a < 2; a++)
                for (int bq = 0; bq < 2; bq++) {
                    int covered = quadrant(face, dirs[i], a, bq);
                    if (covered < 0)
                        throw new IllegalStateException(
                            "partial quadrant " + canonical(st) + " "
                            + dirs[i] + " A" + a + "B" + bq);
                    nib |= covered << (2 * a + bq);
                }
            if (i > 0)
                sb.append(',');
            sb.append(tag[i]).append(':')
              .append(Character.forDigit(nib == 0 ? 0 : nib,
                                         16)); // 아래 조립에서 니블 자체
        }
        // 니블을 hex 로: 0..15 (f). forDigit 이 10..15 → a..f
        String s = sb.toString();
        return s;
    }

    /* 쿼드런트 커버리지: 1 = 전부, 0 = 전무, -1 = 부분 (규약 위반).
     * world-axis 규약: N/S 면 A=y(0=하), B=x(0=서); W/E 면 A=y, B=z(0=북);
     * D/U 면 A=z(0=북), B=x(0=서). */
    static int quadrant(VoxelShape face, Direction d, int a, int b) {
        if (face.isEmpty())
            return 0;
        double a0 = a * 0.5, a1 = a0 + 0.5, b0 = b * 0.5, b1 = b0 + 0.5;
        // 쿼드런트 내 9x9 샘플 격자로 all-or-nothing 판정 (0.5 그리드
        // 정렬 형상이라 충분; 혼합이면 -1)
        int in = 0, tot = 0;
        for (int i = 1; i <= 9; i++)
            for (int j = 1; j <= 9; j++) {
                double pa = a0 + (a1 - a0) * i / 10.0;
                double pb = b0 + (b1 - b0) * j / 10.0;
                double x, y, z;
                switch (d) {
                case NORTH, SOUTH -> {
                    y = pa;
                    x = pb;
                    z = 0.25;
                }
                case WEST, EAST -> {
                    y = pa;
                    z = pb;
                    x = 0.25;
                }
                default -> {
                    z = pa;
                    x = pb;
                    y = 0.25;
                }
                }
                if (contains(face, d, x, y, z))
                    in++;
                tot++;
            }
        if (in == 0)
            return 0;
        if (in == tot)
            return 1;
        return -1;
    }

    static boolean contains(VoxelShape face, Direction d, double x, double y,
                            double z) {
        // face 형상은 면 평면에 붙은 얇은 3D 형상 — 면 법선 축 좌표는
        // AABB 의 실제 범위 중앙으로 치환해 포함 검사
        for (AABB box : face.toAabbs()) {
            double nx = x, ny = y, nz = z;
            switch (d.getAxis()) {
            case X -> nx = (box.minX + box.maxX) / 2.0;
            case Y -> ny = (box.minY + box.maxY) / 2.0;
            case Z -> nz = (box.minZ + box.maxZ) / 2.0;
            }
            if (nx > box.minX - 1e-9 && nx < box.maxX + 1e-9
                && ny > box.minY - 1e-9 && ny < box.maxY + 1e-9
                && nz > box.minZ - 1e-9 && nz < box.maxZ + 1e-9)
                return true;
        }
        return false;
    }
}
