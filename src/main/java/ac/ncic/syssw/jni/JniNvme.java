/*
 * Copyleft 2016, AZQ. All rites reversed.
 */

package ac.ncic.syssw.jni;

import java.nio.ByteBuffer;

public final class JniNvme {
	static {
		System.loadLibrary("jninvme");
	}

	public static native void nvmeInitialize();
	public static native void nvmeFinalize();

	public static native ByteBuffer allocateHugepageMemory(long size);
	public static native void freeHugepageMemory(ByteBuffer buffer);

	public static native void nvmeWrite(ByteBuffer buffer, long offset, long size);
	public static native void nvmeRead(ByteBuffer buffer, long offset, long size);

	//public static native void nvmeWriteAsync(long buffer, long offset, long size);
	//public static native void nvmeReadAsync (long buffer, long offset, long size);
	//public static native void nvmePoll();
}
