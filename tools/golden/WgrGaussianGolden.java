// golden/rng/wgr_gaussian.txt 재생성 — WorldgenRandom(Xoroshiro).nextGaussian
// 패리티. 서버 jar 의 실제 WorldgenRandom 을 구동한다 (deobf 클래스).
// 패턴: setDecorationSeed(1234567890, 16, 32) 후 feature index 0..199 를
// step 7 로 재시드하며 1+(i%3) 개씩 드로우 — 홀수 드로우가 캐시를 물고
// 재시드를 넘어가는 지속 시맨틱 (WorldgenRandom.setSeed 가
// gaussianSource.reset() 을 부르지 않음) 을 정확히 커버한다.
public class WgrGaussianGolden {
    public static void main(String[] args) throws Exception {
        var x = new net.minecraft.world.level.levelgen.XoroshiroRandomSource(1234567890L);
        var r = new net.minecraft.world.level.levelgen.WorldgenRandom(x);
        long deco = r.setDecorationSeed(1234567890L, 16, 32);
        var sb = new StringBuilder();
        sb.append("# WorldgenRandom.nextGaussian golden, JDK ")
          .append(System.getProperty("java.version"))
          .append(", server 26.2 deobf\n");
        sb.append("# setDecorationSeed(1234567890,16,32); i in 0..199: setFeatureSeed(deco,i,7); 1+(i%3) draws\n");
        for (int i = 0; i < 200; i++) {
            r.setFeatureSeed(deco, i, 7);
            int n = 1 + (i % 3);
            for (int k = 0; k < n; k++)
                sb.append(String.format("g=0x%016x%n", Double.doubleToRawLongBits(r.nextGaussian())));
        }
        java.nio.file.Files.writeString(java.nio.file.Path.of(args[0], "wgr_gaussian.txt"), sb.toString());
        System.out.println("wrote wgr_gaussian.txt");
    }
}
