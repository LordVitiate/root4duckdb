import java.sql.Connection;
import java.sql.DriverManager;
import java.sql.ResultSet;
import java.sql.Statement;
import java.util.ArrayList;
import java.util.List;
import java.util.Properties;
import java.util.concurrent.Callable;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.Future;
import java.util.concurrent.TimeUnit;

public final class JdbcRootIntegration {
    private record Result(long rows, double sum) {}

    private static String sqlQuote(String value) {
        return value.replace("'", "''");
    }

    private static Result run(String mode, String initFile, String dataFile, String dictionary) throws Exception {
        Properties settings = new Properties();
        settings.setProperty("allow_unsigned_extensions", "true");
        settings.setProperty("session_init_sql_file", initFile);
        try (Connection connection = DriverManager.getConnection("jdbc:duckdb:", settings);
             Statement statement = connection.createStatement()) {
            try (ResultSet functions = statement.executeQuery("SELECT count(*) FROM duckdb_functions() WHERE function_name = 'read_root'")) {
                if (!functions.next() || functions.getLong(1) < 1) {
                    throw new IllegalStateException("JDBC connection has no read_root function after LOAD");
                }
            }
            if (mode.equals("load")) {
                return new Result(0, 0);
            }
            String sql = "SELECT count(*), coalesce(sum(value), 0) FROM read_root('" + sqlQuote(dataFile)
                    + "', dictionary := '" + sqlQuote(dictionary)
                    + "', path_prefix := '/PaEvent/vecTrack/chi2tot', reader_mode := '" + mode
                    + "') WHERE entry_id BETWEEN 929 AND 933";
            Result baseline = null;
            for (int iteration = 0; iteration < 6; ++iteration) {
                try (ResultSet rows = statement.executeQuery(sql)) {
                    if (!rows.next()) {
                        throw new IllegalStateException("ROOT query returned no aggregate row");
                    }
                    Result current = new Result(rows.getLong(1), rows.getDouble(2));
                    if (current.rows() < 1 || !Double.isFinite(current.sum())) {
                        throw new IllegalStateException("ROOT query returned invalid data: " + current);
                    }
                    if (baseline != null) {
                        verify(baseline, current);
                    }
                    baseline = current;
                }
            }
            return baseline;
        }
    }

    private static void verify(Result expected, Result actual) {
        double tolerance = 1e-8 * Math.max(1.0, Math.abs(expected.sum()));
        if (expected.rows() != actual.rows() || Math.abs(expected.sum() - actual.sum()) > tolerance) {
            throw new IllegalStateException("Different ROOT results: " + expected + " vs " + actual);
        }
    }

    public static void main(String[] args) throws Exception {
        if (args.length != 5) {
            throw new IllegalArgumentException("Usage: JdbcRootIntegration <load|serialized|object> <workers> <init.sql> <mDST.root> <libPhast.so>");
        }
        String mode = args[0];
        if (!mode.equals("load") && !mode.equals("serialized") && !mode.equals("object")) {
            throw new IllegalArgumentException("Unsupported mode: " + mode);
        }
        int workers = Integer.parseInt(args[1]);
        if (workers < 1 || workers > 64) {
            throw new IllegalArgumentException("Workers must be in [1, 64]");
        }
        CountDownLatch ready = new CountDownLatch(workers);
        CountDownLatch start = new CountDownLatch(1);
        ExecutorService executor = Executors.newFixedThreadPool(workers);
        try {
            List<Future<Result>> futures = new ArrayList<>();
            for (int worker = 0; worker < workers; ++worker) {
                Callable<Result> task = () -> {
                    ready.countDown();
                    start.await();
                    return run(mode, args[2], args[3], args[4]);
                };
                futures.add(executor.submit(task));
            }
            if (!ready.await(30, TimeUnit.SECONDS)) {
                throw new IllegalStateException("JDBC workers did not reach the starting barrier");
            }
            start.countDown();
            Result first = null;
            for (Future<Result> future : futures) {
                Result current = future.get(180, TimeUnit.SECONDS);
                if (first != null) {
                    verify(first, current);
                }
                first = current;
            }
            System.out.println("[OK] JDBC " + mode + " workers=" + workers + " result=" + first);
        } finally {
            start.countDown();
            executor.shutdownNow();
        }
    }
}
