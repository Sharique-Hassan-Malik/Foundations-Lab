package lsm;

import java.io.BufferedInputStream;
import java.io.BufferedOutputStream;
import java.io.Closeable;
import java.io.DataInputStream;
import java.io.DataOutputStream;
import java.io.EOFException;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.nio.charset.StandardCharsets;

/**
 * The write-ahead log — the durability half of the LSM tree.
 *
 * Every put and delete is appended here and flushed to the OS *before* it is
 * applied to the in-memory memtable. So if the process dies with a full memtable
 * that was never flushed to an SSTable, reopening the tree replays the log and
 * recovers every acknowledged write. On a clean flush the log is truncated,
 * because those writes now live durably in an SSTable instead.
 *
 * A record is: op byte (0 = put, 1 = delete), key length + bytes, and for a put
 * the value length + bytes. Replay tolerates a torn trailing record — the
 * signature of a crash mid-append — by stopping at the first short read.
 */
public final class WriteAheadLog implements Closeable {
    private final File file;
    private DataOutputStream out;

    public WriteAheadLog(File file) throws IOException {
        this.file = file;
        this.out = new DataOutputStream(new BufferedOutputStream(new FileOutputStream(file, true)));
    }

    public synchronized void append(String key, byte[] value, boolean tombstone) throws IOException {
        byte[] k = key.getBytes(StandardCharsets.UTF_8);
        out.writeByte(tombstone ? 1 : 0);
        out.writeInt(k.length);
        out.write(k);
        if (!tombstone) {
            out.writeInt(value.length);
            out.write(value);
        }
        out.flush();                 // durable to the OS before we return
    }

    /** Start a fresh, empty log (called after the memtable is flushed to disk). */
    public synchronized void truncate() throws IOException {
        out.close();
        this.out = new DataOutputStream(new BufferedOutputStream(new FileOutputStream(file, false)));
    }

    @Override
    public synchronized void close() throws IOException {
        out.close();
    }

    public interface Handler {
        void handle(String key, byte[] value, boolean tombstone);
    }

    /** Replay a log file into a handler, stopping cleanly at a torn final record. */
    public static void replay(File file, Handler handler) throws IOException {
        if (!file.exists()) return;
        try (DataInputStream in = new DataInputStream(new BufferedInputStream(new FileInputStream(file)))) {
            while (true) {
                int op;
                try {
                    op = in.readByte();
                } catch (EOFException eof) {
                    break;
                }
                int klen;
                try {
                    klen = in.readInt();
                } catch (EOFException eof) {
                    break;
                }
                byte[] k = in.readNBytes(klen);
                if (k.length < klen) break;                      // torn record
                String key = new String(k, StandardCharsets.UTF_8);
                if (op == 1) {
                    handler.handle(key, null, true);
                } else {
                    int vlen;
                    try {
                        vlen = in.readInt();
                    } catch (EOFException eof) {
                        break;
                    }
                    byte[] v = in.readNBytes(vlen);
                    if (v.length < vlen) break;                  // torn record
                    handler.handle(key, v, false);
                }
            }
        }
    }
}
