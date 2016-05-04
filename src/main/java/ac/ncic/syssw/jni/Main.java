/*
 * Copyleft 2016, AZQ. All rites reversed.
 */

package ac.ncic.syssw.jni;

import java.nio.ByteBuffer;
import java.util.Random;

public class Main {
	public static void main(String[] args) {
		JniNvme.nvmeInitialize();

		ByteBuffer bufferW = JniNvme.allocateHugepageMemory(512);
		ByteBuffer bufferR = JniNvme.allocateHugepageMemory(512);

		byte[] data = new byte[512];
		(new Random()).nextBytes(data);
		bufferW.put(data);

		System.out.println("write, read, compare ...");

		JniNvme.nvmeWrite(bufferW, 0, 512);
		JniNvme.nvmeRead (bufferR, 0, 512);

		bufferW.flip();
		bufferR.flip();
		if (bufferR.compareTo(bufferW) == 0) {
			System.out.println("YES!");
		}

		JniNvme.freeHugepageMemory(bufferW);
		JniNvme.freeHugepageMemory(bufferR);

		JniNvme.nvmeFinalize();
	}
}
