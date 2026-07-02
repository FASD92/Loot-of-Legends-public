package com.lol.meta.firewall;

import static org.assertj.core.api.Assertions.assertThat;

import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.net.StandardProtocolFamily;
import java.net.UnixDomainSocketAddress;
import java.nio.ByteBuffer;
import java.nio.channels.ServerSocketChannel;
import java.nio.channels.SocketChannel;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.time.Duration;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicReference;
import org.junit.jupiter.api.Test;
import org.junit.jupiter.api.io.TempDir;

class GameFirewallClientTests {

  @TempDir private Path tempDir;

  @Test
  void noopClientReturnsSuccessWithoutSocketIo() {
    GameFirewallClient client = new NoopGameFirewallClient();

    GameFirewallClient.Decision decision =
        client.allowPreAuth("203.0.113.7", Duration.ofSeconds(60), "pre-auth");

    assertThat(decision.success()).isTrue();
    assertThat(decision.errorCode()).isEmpty();
  }

  @Test
  void unixSocketClientSerializesAllowRequestAsOneJsonLine() throws Exception {
    Path socketPath = tempDir.resolve("firewall-agent.sock");
    SingleResponseServer server =
        SingleResponseServer.start(
            socketPath, "{\"success\":true,\"errorCode\":\"\",\"message\":\"\"}\n");
    UnixSocketGameFirewallClient client =
        new UnixSocketGameFirewallClient(
            socketPath, Duration.ofMillis(500), Duration.ofMillis(500));

    GameFirewallClient.Decision decision =
        client.allowPreAuth("203.0.113.7", Duration.ofSeconds(60), "pre-auth");

    assertThat(decision.success()).isTrue();
    assertThat(server.awaitRequest())
        .isEqualTo(
            "{\"op\":\"allow\",\"clientIp\":\"203.0.113.7\","
                + "\"ttlSeconds\":60,\"reason\":\"pre-auth\"}");
  }

  @Test
  void unixSocketClientParsesAgentErrorResponse() throws Exception {
    Path socketPath = tempDir.resolve("firewall-agent.sock");
    SingleResponseServer server =
        SingleResponseServer.start(
            socketPath,
            "{\"success\":false,\"errorCode\":\"InvalidClientIp\","
                + "\"message\":\"clientIp must be IPv4 literal\"}\n");
    UnixSocketGameFirewallClient client =
        new UnixSocketGameFirewallClient(
            socketPath, Duration.ofMillis(500), Duration.ofMillis(500));

    GameFirewallClient.Decision decision =
        client.allowPreAuth("203.0.113.8", Duration.ofSeconds(60), "pre-auth");

    assertThat(server.awaitRequest()).contains("\"clientIp\":\"203.0.113.8\"");
    assertThat(decision.success()).isFalse();
    assertThat(decision.errorCode()).isEqualTo("InvalidClientIp");
    assertThat(decision.message()).contains("IPv4");
  }

  @Test
  void unixSocketClientReturnsFailureForMalformedResponse() throws Exception {
    Path socketPath = tempDir.resolve("firewall-agent.sock");
    SingleResponseServer.start(socketPath, "not-json\n");
    UnixSocketGameFirewallClient client =
        new UnixSocketGameFirewallClient(
            socketPath, Duration.ofMillis(500), Duration.ofMillis(500));

    GameFirewallClient.Decision decision =
        client.allowPreAuth("203.0.113.9", Duration.ofSeconds(60), "pre-auth");

    assertThat(decision.success()).isFalse();
    assertThat(decision.errorCode()).isEqualTo("FirewallAgentUnavailable");
  }

  @Test
  void unixSocketClientReturnsFailureForReadTimeout() throws Exception {
    Path socketPath = tempDir.resolve("firewall-agent.sock");
    HangingServer.start(socketPath);
    UnixSocketGameFirewallClient client =
        new UnixSocketGameFirewallClient(
            socketPath, Duration.ofMillis(500), Duration.ofMillis(50));

    GameFirewallClient.Decision decision =
        client.allowPreAuth("203.0.113.10", Duration.ofSeconds(60), "pre-auth");

    assertThat(decision.success()).isFalse();
    assertThat(decision.errorCode()).isEqualTo("FirewallAgentUnavailable");
  }

  private static String readLine(SocketChannel channel) throws IOException {
    ByteArrayOutputStream output = new ByteArrayOutputStream();
    ByteBuffer buffer = ByteBuffer.allocate(1);
    while (true) {
      buffer.clear();
      int read = channel.read(buffer);
      if (read < 0) {
        break;
      }
      buffer.flip();
      byte value = buffer.get();
      if (value == '\n') {
        break;
      }
      output.write(value);
    }
    return output.toString(StandardCharsets.UTF_8);
  }

  private static void writeLine(SocketChannel channel, String response) throws IOException {
    channel.write(ByteBuffer.wrap(response.getBytes(StandardCharsets.UTF_8)));
  }

  private static final class SingleResponseServer {
    private final AtomicReference<String> request = new AtomicReference<>();
    private final AtomicReference<Throwable> failure = new AtomicReference<>();
    private final CountDownLatch done = new CountDownLatch(1);

    static SingleResponseServer start(Path socketPath, String response) throws Exception {
      SingleResponseServer server = new SingleResponseServer();
      CountDownLatch ready = new CountDownLatch(1);
      Thread thread =
          new Thread(
              () -> {
                try {
                  Files.deleteIfExists(socketPath);
                  try (ServerSocketChannel listener =
                      ServerSocketChannel.open(StandardProtocolFamily.UNIX)) {
                    listener.bind(UnixDomainSocketAddress.of(socketPath));
                    ready.countDown();
                    try (SocketChannel channel = listener.accept()) {
                      server.request.set(readLine(channel));
                      writeLine(channel, response);
                    }
                  }
                } catch (Throwable error) {
                  server.failure.set(error);
                } finally {
                  server.done.countDown();
                }
              });
      thread.setDaemon(true);
      thread.start();
      assertThat(ready.await(2, TimeUnit.SECONDS)).isTrue();
      return server;
    }

    String awaitRequest() throws Exception {
      assertThat(done.await(2, TimeUnit.SECONDS)).isTrue();
      assertThat(failure.get()).isNull();
      return request.get();
    }
  }

  private static final class HangingServer {
    static void start(Path socketPath) throws Exception {
      CountDownLatch ready = new CountDownLatch(1);
      Thread thread =
          new Thread(
              () -> {
                try {
                  Files.deleteIfExists(socketPath);
                  try (ServerSocketChannel listener =
                      ServerSocketChannel.open(StandardProtocolFamily.UNIX)) {
                    listener.bind(UnixDomainSocketAddress.of(socketPath));
                    ready.countDown();
                    try (SocketChannel channel = listener.accept()) {
                      readLine(channel);
                      Thread.sleep(500);
                    }
                  }
                } catch (IOException | InterruptedException ignored) {
                  Thread.currentThread().interrupt();
                }
              });
      thread.setDaemon(true);
      thread.start();
      assertThat(ready.await(2, TimeUnit.SECONDS)).isTrue();
    }
  }
}
