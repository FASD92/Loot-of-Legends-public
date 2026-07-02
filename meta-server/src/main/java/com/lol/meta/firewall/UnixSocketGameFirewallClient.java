package com.lol.meta.firewall;

import com.fasterxml.jackson.databind.JsonNode;
import com.fasterxml.jackson.databind.ObjectMapper;
import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.net.SocketTimeoutException;
import java.net.StandardProtocolFamily;
import java.net.UnixDomainSocketAddress;
import java.nio.ByteBuffer;
import java.nio.channels.SelectionKey;
import java.nio.channels.Selector;
import java.nio.channels.SocketChannel;
import java.nio.charset.StandardCharsets;
import java.nio.file.Path;
import java.time.Duration;
import java.util.LinkedHashMap;
import java.util.Map;
import java.util.Objects;

public final class UnixSocketGameFirewallClient implements GameFirewallClient {

  private static final int MAX_RESPONSE_BYTES = 4096;
  private static final String UNAVAILABLE = "FirewallAgentUnavailable";

  private final Path socketPath;
  private final Duration connectTimeout;
  private final Duration readTimeout;
  private final ObjectMapper objectMapper;

  public UnixSocketGameFirewallClient(
      Path socketPath, Duration connectTimeout, Duration readTimeout) {
    this(socketPath, connectTimeout, readTimeout, new ObjectMapper());
  }

  UnixSocketGameFirewallClient(
      Path socketPath,
      Duration connectTimeout,
      Duration readTimeout,
      ObjectMapper objectMapper) {
    this.socketPath = Objects.requireNonNull(socketPath, "socketPath");
    this.connectTimeout = positive(connectTimeout, "connectTimeout");
    this.readTimeout = positive(readTimeout, "readTimeout");
    this.objectMapper = Objects.requireNonNull(objectMapper, "objectMapper");
  }

  @Override
  public Decision allowPreAuth(String clientIp, Duration ttl, String reason) {
    Map<String, Object> payload = new LinkedHashMap<>();
    payload.put("op", "allow");
    payload.put("clientIp", clientIp);
    payload.put("ttlSeconds", ttlSeconds(ttl));
    payload.put("reason", reason);
    return exchange(payload);
  }

  @Override
  public Decision renewActiveSession(String clientIp, Duration ttl, String sessionId) {
    Map<String, Object> payload = new LinkedHashMap<>();
    payload.put("op", "renew");
    payload.put("clientIp", clientIp);
    payload.put("ttlSeconds", ttlSeconds(ttl));
    payload.put("sessionId", sessionId);
    return exchange(payload);
  }

  @Override
  public Decision closeClientIp(String clientIp) {
    Map<String, Object> payload = new LinkedHashMap<>();
    payload.put("op", "close");
    payload.put("clientIp", clientIp);
    payload.put("scope", "single");
    return exchange(payload);
  }

  @Override
  public Decision closeSession(String sessionId) {
    Map<String, Object> payload = new LinkedHashMap<>();
    payload.put("op", "close");
    payload.put("sessionId", sessionId);
    payload.put("scope", "single");
    return exchange(payload);
  }

  private Decision exchange(Map<String, Object> payload) {
    try {
      String request = objectMapper.writeValueAsString(payload) + "\n";
      String response = sendAndReceive(request);
      return parseDecision(response);
    } catch (Exception error) {
      return Decision.failure(UNAVAILABLE, "firewall agent request failed");
    }
  }

  private String sendAndReceive(String request)
      throws IOException, SocketTimeoutException {
    byte[] requestBytes = request.getBytes(StandardCharsets.UTF_8);
    try (SocketChannel channel = SocketChannel.open(StandardProtocolFamily.UNIX);
        Selector selector = Selector.open()) {
      channel.configureBlocking(false);
      SelectionKey key = channel.register(selector, 0);
      channel.connect(UnixDomainSocketAddress.of(socketPath));
      finishConnect(channel, selector, key);
      writeAll(channel, selector, key, ByteBuffer.wrap(requestBytes));
      return readLine(channel, selector, key);
    }
  }

  private void finishConnect(SocketChannel channel, Selector selector, SelectionKey key)
      throws IOException {
    long deadline = deadlineNanos(connectTimeout);
    while (!channel.finishConnect()) {
      waitUntilReady(selector, key, SelectionKey.OP_CONNECT, remaining(deadline));
    }
  }

  private void writeAll(
      SocketChannel channel, Selector selector, SelectionKey key, ByteBuffer buffer)
      throws IOException {
    long deadline = deadlineNanos(connectTimeout);
    while (buffer.hasRemaining()) {
      int written = channel.write(buffer);
      if (written == 0) {
        waitUntilReady(selector, key, SelectionKey.OP_WRITE, remaining(deadline));
      }
    }
  }

  private String readLine(SocketChannel channel, Selector selector, SelectionKey key)
      throws IOException {
    long deadline = deadlineNanos(readTimeout);
    ByteArrayOutputStream output = new ByteArrayOutputStream();
    ByteBuffer buffer = ByteBuffer.allocate(256);
    while (true) {
      buffer.clear();
      int read = channel.read(buffer);
      if (read < 0) {
        break;
      }
      if (read == 0) {
        waitUntilReady(selector, key, SelectionKey.OP_READ, remaining(deadline));
        continue;
      }
      buffer.flip();
      while (buffer.hasRemaining()) {
        byte value = buffer.get();
        if (value == '\n') {
          return output.toString(StandardCharsets.UTF_8);
        }
        output.write(value);
        if (output.size() > MAX_RESPONSE_BYTES) {
          throw new IOException("firewall agent response too large");
        }
      }
    }
    return output.toString(StandardCharsets.UTF_8);
  }

  private void waitUntilReady(
      Selector selector, SelectionKey key, int operation, Duration timeout)
      throws IOException {
    key.interestOps(operation);
    int ready = selector.select(Math.max(1L, timeout.toMillis()));
    selector.selectedKeys().clear();
    key.interestOps(0);
    if (ready == 0) {
      throw new SocketTimeoutException("firewall agent timed out");
    }
  }

  private Decision parseDecision(String response) throws IOException {
    JsonNode root = objectMapper.readTree(response);
    JsonNode successNode = root.get("success");
    if (successNode == null || !successNode.isBoolean()) {
      throw new IOException("firewall agent response missing success flag");
    }
    if (successNode.booleanValue()) {
      return Decision.ok();
    }
    return Decision.failure(
        text(root, "errorCode", "FirewallAgentError"),
        text(root, "message", "firewall agent rejected request"));
  }

  private static String text(JsonNode root, String field, String fallback) {
    JsonNode node = root.get(field);
    if (node == null || !node.isTextual()) {
      return fallback;
    }
    String value = node.asText();
    return value.isBlank() ? fallback : value;
  }

  private static int ttlSeconds(Duration ttl) {
    Objects.requireNonNull(ttl, "ttl");
    return Math.toIntExact(ttl.toSeconds());
  }

  private static Duration positive(Duration value, String name) {
    Objects.requireNonNull(value, name);
    if (value.isZero() || value.isNegative()) {
      throw new IllegalArgumentException(name + " must be positive");
    }
    return value;
  }

  private static long deadlineNanos(Duration timeout) {
    return System.nanoTime() + timeout.toNanos();
  }

  private static Duration remaining(long deadlineNanos) throws SocketTimeoutException {
    long remaining = deadlineNanos - System.nanoTime();
    if (remaining <= 0) {
      throw new SocketTimeoutException("firewall agent timed out");
    }
    return Duration.ofNanos(remaining);
  }
}
