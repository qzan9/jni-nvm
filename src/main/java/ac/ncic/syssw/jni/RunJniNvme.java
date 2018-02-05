/*
 * Copyleft 2016, AZQ. All rites reversed.
 */

package ac.ncic.syssw.jni;

import java.nio.ByteBuffer;
import java.util.Random;

public class RunJniNvme {

	public static final int U2_IO_NUMBER = 8192;
	public static final long U2_NS_SIZE = 400000000000L;

	public static final int U2_IO_SIZE_MIN = 512;
	public static final int U2_IO_SIZE_MAX = 4194304;

	private RunJniNvme() { }
	private static RunJniNvme INSTANCE;
	public static RunJniNvme getInstance() {
		if (null == INSTANCE) {
			INSTANCE = new RunJniNvme();
		}
		return INSTANCE;
	}

	public void helloWorldJniNvme() {
		JniNvme.nvmeInitialize();

		System.out.println("[helloWorldJniNvme]");

		ByteBuffer writeBuffer = JniNvme.allocateHugepageMemory(4096);
		ByteBuffer readBuffer  = JniNvme.allocateHugepageMemory(4096);

		byte[] data = new byte[4096];
		(new Random()).nextBytes(data);
		writeBuffer.put(data);

		JniNvme.nvmeWrite(writeBuffer, 4096, 4096);
		JniNvme.nvmeRead (readBuffer, 4096, 4096);

		writeBuffer.rewind();
		readBuffer.rewind();
		System.out.println(readBuffer.compareTo(writeBuffer));

		JniNvme.freeHugepageMemory(writeBuffer);
		JniNvme.freeHugepageMemory(readBuffer);

		JniNvme.nvmeFinalize();
	}

	public void latencyBenchmarkJniNvme() {
		JniNvme.nvmeInitialize();

		System.out.println("[latencyBenchmarkJniNvme]");

		System.out.printf("u2-java latency benchmarking ... RW type: sequential read, IOs: %d\n", U2_IO_NUMBER);
		System.out.printf("\t%8s\t\t%12s\t\t%12s\n", "I/O size", "latency", "elapsed time");

		long offset = 0;
		long elapsedTime = 0;
		long startTime = 0;
		for (int ioSize = U2_IO_SIZE_MIN; ioSize <= U2_IO_SIZE_MAX; ioSize *= 2) {
			System.out.printf("\t%8d", ioSize);

			ByteBuffer buffer = JniNvme.allocateHugepageMemory(ioSize);

			startTime = System.nanoTime();
			for (int i = 0; i < U2_IO_NUMBER; i++) {
				JniNvme.nvmeRead(buffer, offset, ioSize);
				offset += ioSize;
				if (offset > U2_NS_SIZE - ioSize) {
					offset = 0;
				}
			}
			elapsedTime = System.nanoTime() - startTime;

			System.out.printf("\t\t%9.1f us", (float) elapsedTime / 1000 / U2_IO_NUMBER);
			System.out.printf("\t\t%10.1f s", (float) elapsedTime / 1000000000);
			System.out.printf("\n");

			JniNvme.freeHugepageMemory(buffer);
		}

		JniNvme.nvmeFinalize();
	}
}
