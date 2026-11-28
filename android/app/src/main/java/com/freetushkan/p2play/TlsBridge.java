package com.freetushkan.p2play;

import android.util.Log;

import java.io.BufferedInputStream;
import java.io.ByteArrayOutputStream;
import java.io.DataInputStream;
import java.io.DataOutputStream;
import java.io.EOFException;
import java.io.IOException;
import java.net.Inet4Address;
import java.net.InetAddress;
import java.net.InetSocketAddress;
import java.net.ServerSocket;
import java.net.Socket;
import java.net.SocketException;
import java.net.URI;
import java.net.UnknownHostException;
import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;
import java.security.SecureRandom;
import java.util.Locale;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.TimeUnit;
import javax.net.ssl.HttpsURLConnection;
import javax.net.ssl.SSLHandshakeException;
import javax.net.ssl.SSLSocket;
import javax.net.ssl.SSLSocketFactory;

/**
 * Authenticated loopback-only HTTPS bridge for p2playd. TLS is terminated by Android's
 * platform SSLSocket and system trust store. The native core supplies the URL and the
 * already-validated DNS pin; redirects are returned to the core for a fresh SSRF check.
 */
final class TlsBridge implements AutoCloseable {
    private static final String TAG = "P2PlayTLS";
    private static final byte[] REQUEST_MAGIC = "P2PTLS01".getBytes(StandardCharsets.US_ASCII);
    private static final byte[] RESPONSE_MAGIC = "P2PTLR01".getBytes(StandardCharsets.US_ASCII);
    private static final int MAX_URL = 2048;
    private static final int MAX_RESOLVE = 512;
    private static final int MAX_HEADER_BYTES = 32 * 1024;
    private static final int MAX_HEADER_LINE = 8 * 1024;
    private static final int MAX_RESPONSE = 16 * 1024 * 1024;

    private final byte[] tokenBytes;
    private final String token;
    private final ExecutorService workers = Executors.newFixedThreadPool(2);
    private volatile boolean running;
    private ServerSocket listener;
    private Thread acceptThread;

    TlsBridge() throws IOException {
        byte[] random = new byte[16];
        new SecureRandom().nextBytes(random);
        StringBuilder hex = new StringBuilder(32);
        for (byte value : random) hex.append(String.format(Locale.ROOT, "%02x", value & 0xff));
        token = hex.toString();
        tokenBytes = token.getBytes(StandardCharsets.US_ASCII);
    }

    synchronized int start() throws IOException {
        if (running) return listener.getLocalPort();
        ServerSocket server = new ServerSocket();
        server.setReuseAddress(false);
        server.bind(new InetSocketAddress(InetAddress.getByName("127.0.0.1"), 0), 8);
        listener = server;
        running = true;
        acceptThread = new Thread(this::acceptLoop, "p2play-https-bridge");
        acceptThread.setDaemon(true);
        acceptThread.start();
        return server.getLocalPort();
    }

    String token() { return token; }

    private void acceptLoop() {
        while (running) {
            try {
                Socket client = listener.accept();
                client.setSoTimeout(25_000);
                workers.execute(() -> serve(client));
            } catch (SocketException e) {
                if (running) Log.w(TAG, "TLS bridge accept failed", e);
                return;
            } catch (IOException | RuntimeException e) {
                if (running) Log.w(TAG, "TLS bridge accept failed", e);
            }
        }
    }

    private void serve(Socket client) {
        try (Socket socket = client;
             DataInputStream input = new DataInputStream(new BufferedInputStream(socket.getInputStream()));
             DataOutputStream output = new DataOutputStream(socket.getOutputStream())) {
            byte[] magic = new byte[REQUEST_MAGIC.length];
            input.readFully(magic);
            if (!MessageDigest.isEqual(magic, REQUEST_MAGIC)) return;

            byte[] suppliedToken = new byte[tokenBytes.length];
            input.readFully(suppliedToken);
            if (!MessageDigest.isEqual(suppliedToken, tokenBytes)) {
                Log.w(TAG, "Rejected unauthenticated local TLS bridge request");
                return;
            }

            int urlLength = input.readUnsignedShort();
            int resolveLength = input.readUnsignedShort();
            long capacity = Integer.toUnsignedLong(input.readInt());
            if (urlLength == 0 || urlLength > MAX_URL || resolveLength == 0 ||
                    resolveLength > MAX_RESOLVE || capacity == 0 || capacity > MAX_RESPONSE) {
                writeResponse(output, new Result(0, new byte[0], "", "",
                        "invalid HTTPS bridge request limits"));
                return;
            }

            byte[] urlBytes = new byte[urlLength];
            byte[] resolveBytes = new byte[resolveLength];
            input.readFully(urlBytes);
            input.readFully(resolveBytes);
            String url = new String(urlBytes, StandardCharsets.UTF_8);
            String pinned = new String(resolveBytes, StandardCharsets.UTF_8);

            Result result;
            try {
                result = perform(url, pinned, (int) capacity);
            } catch (Exception e) {
                Log.w(TAG, "HTTPS poster request failed: " + e.getMessage());
                result = new Result(0, new byte[0], "", "", safeMessage(e));
            }
            writeResponse(output, result);
        } catch (EOFException ignored) {
            // The daemon stopped or the client closed an incomplete request.
        } catch (IOException e) {
            if (running) Log.w(TAG, "TLS bridge connection failed", e);
        }
    }

    private static String safeMessage(Exception e) {
        String message = e.getMessage();
        if (message == null || message.trim().isEmpty()) message = e.getClass().getSimpleName();
        return message.length() > 2000 ? message.substring(0, 2000) : message;
    }

    private static final class Result {
        final int status;
        final byte[] body;
        final String contentType;
        final String location;
        final String error;

        Result(int status, byte[] body, String contentType, String location, String error) {
            this.status = status;
            this.body = body;
            this.contentType = contentType;
            this.location = location;
            this.error = error;
        }
    }

    private static Result perform(String value, String pinned, int maxBytes) throws Exception {
        URI uri = new URI(value);
        boolean https = "https".equalsIgnoreCase(uri.getScheme());
        boolean http = "http".equalsIgnoreCase(uri.getScheme());
        if ((!https && !http) || uri.getRawUserInfo() != null ||
                uri.getHost() == null || uri.getHost().isEmpty() || uri.getRawFragment() != null) {
            throw new IOException("invalid HTTP(S) URL");
        }
        String host = uri.getHost();
        int port = uri.getPort() < 0 ? (https ? 443 : 80) : uri.getPort();
        if (port < 1 || port > 65535) throw new IOException("invalid HTTPS port");

        String[] parts = pinned.split(":", -1);
        if (parts.length != 3 || !host.equalsIgnoreCase(parts[0])) {
            throw new IOException("DNS pin does not match the HTTPS host");
        }
        int pinnedPort;
        try {
            pinnedPort = Integer.parseInt(parts[1]);
        } catch (NumberFormatException e) {
            throw new IOException("invalid DNS pin port");
        }
        if (pinnedPort != port) throw new IOException("DNS pin does not match the HTTPS port");
        InetAddress pinnedAddress = InetAddress.getByName(parts[2]);
        if (!(pinnedAddress instanceof Inet4Address) || pinnedAddress.isAnyLocalAddress() ||
                pinnedAddress.isLoopbackAddress() || pinnedAddress.isLinkLocalAddress() ||
                pinnedAddress.isSiteLocalAddress() || pinnedAddress.isMulticastAddress()) {
            throw new IOException("DNS pin is not a public IPv4 address");
        }

        Socket tcp = new Socket();
        tcp.connect(new InetSocketAddress(pinnedAddress, port), 10_000);
        tcp.setSoTimeout(15_000);
        SSLSocket tls = null;
        Socket connection = tcp;
        try {
            if (https) {
                SSLSocketFactory factory = (SSLSocketFactory) SSLSocketFactory.getDefault();
                tls = (SSLSocket) factory.createSocket(tcp, host, port, true);
                tls.setUseClientMode(true);
                // Android added SSLParameters.setEndpointIdentificationAlgorithm in API 24.
                // Keep API 23 support: verify the peer name explicitly after handshake
                // and before sending any HTTP request bytes.
                tls.startHandshake();
                if (!HttpsURLConnection.getDefaultHostnameVerifier().verify(host, tls.getSession())) {
                    throw new SSLHandshakeException("HTTPS certificate hostname verification failed");
                }
                connection = tls;
            }

            String path = uri.getRawPath();
            if (path == null || path.isEmpty()) path = "/";
            if (uri.getRawQuery() != null) path += "?" + uri.getRawQuery();
            String hostHeader = host.indexOf(':') >= 0 ? "[" + host + "]" : host;
            if (port != 443) hostHeader += ":" + port;
            String request = "GET " + path + " HTTP/1.1\r\n" +
                    "Host: " + hostHeader + "\r\n" +
                    "User-Agent: P2Play/0.1\r\n" +
                    "Accept: */*\r\n" +
                    "Connection: close\r\n\r\n";
            connection.getOutputStream().write(request.getBytes(StandardCharsets.US_ASCII));
            connection.getOutputStream().flush();

            BufferedInputStream response = new BufferedInputStream(connection.getInputStream());
            String statusLine = readLine(response, MAX_HEADER_LINE);
            String[] statusParts = statusLine.split(" ", 3);
            if (statusParts.length < 2 || !statusParts[0].startsWith("HTTP/")) {
                throw new IOException("invalid HTTPS response status line");
            }
            int status;
            try {
                status = Integer.parseInt(statusParts[1]);
            } catch (NumberFormatException e) {
                throw new IOException("invalid HTTPS response status");
            }

            int headerBytes = statusLine.length() + 2;
            long contentLength = -1;
            boolean chunked = false;
            String contentType = "";
            String location = "";
            while (true) {
                String line = readLine(response, MAX_HEADER_LINE);
                headerBytes += line.length() + 2;
                if (headerBytes > MAX_HEADER_BYTES) throw new IOException("HTTPS response headers are too large");
                if (line.isEmpty()) break;
                int colon = line.indexOf(':');
                if (colon <= 0) continue;
                String name = line.substring(0, colon).trim().toLowerCase(Locale.ROOT);
                String headerValue = line.substring(colon + 1).trim();
                if ("content-type".equals(name)) contentType = headerValue;
                else if ("location".equals(name)) location = headerValue;
                else if ("content-length".equals(name)) {
                    try {
                        contentLength = Long.parseLong(headerValue);
                    } catch (NumberFormatException e) {
                        throw new IOException("invalid HTTPS Content-Length");
                    }
                } else if ("transfer-encoding".equals(name) &&
                        headerValue.toLowerCase(Locale.ROOT).contains("chunked")) {
                    chunked = true;
                }
            }

            byte[] body;
            if (chunked) {
                body = readChunked(response, maxBytes);
            } else if (contentLength >= 0) {
                if (contentLength > maxBytes) throw new IOException("HTTP(S) response exceeds the configured size limit");
                body = readExact(response, (int) contentLength);
            } else {
                body = readUntilEof(response, maxBytes);
            }
            return new Result(status, body, contentType, location, "");
        } finally {
            if (tls != null) {
                try { tls.close(); } catch (IOException ignored) { }
            } else {
                try { tcp.close(); } catch (IOException ignored) { }
            }
        }
    }

    private static String readLine(BufferedInputStream input, int limit) throws IOException {
        ByteArrayOutputStream line = new ByteArrayOutputStream();
        int previous = -1;
        while (line.size() <= limit) {
            int value = input.read();
            if (value < 0) throw new EOFException("unexpected EOF in HTTPS headers");
            if (previous == '\r' && value == '\n') {
                byte[] bytes = line.toByteArray();
                return new String(bytes, 0, Math.max(0, bytes.length - 1), StandardCharsets.ISO_8859_1);
            }
            line.write(value);
            previous = value;
        }
        throw new IOException("HTTPS response header line is too long");
    }

    private static byte[] readExact(BufferedInputStream input, int length) throws IOException {
        byte[] body = new byte[length];
        int offset = 0;
        while (offset < length) {
            int count = input.read(body, offset, length - offset);
            if (count < 0) throw new EOFException("truncated HTTPS response body");
            offset += count;
        }
        return body;
    }

    private static byte[] readUntilEof(BufferedInputStream input, int limit) throws IOException {
        ByteArrayOutputStream output = new ByteArrayOutputStream(Math.min(limit, 16 * 1024));
        byte[] buffer = new byte[8192];
        int count;
        while ((count = input.read(buffer)) >= 0) {
            if (output.size() + count > limit) throw new IOException("HTTPS response exceeds the poster size limit");
            output.write(buffer, 0, count);
        }
        return output.toByteArray();
    }

    private static byte[] readChunked(BufferedInputStream input, int limit) throws IOException {
        ByteArrayOutputStream output = new ByteArrayOutputStream(Math.min(limit, 16 * 1024));
        while (true) {
            String line = readLine(input, MAX_HEADER_LINE);
            int semicolon = line.indexOf(';');
            String sizeText = (semicolon >= 0 ? line.substring(0, semicolon) : line).trim();
            long size;
            try {
                size = Long.parseLong(sizeText, 16);
            } catch (NumberFormatException e) {
                throw new IOException("invalid HTTPS chunk size");
            }
            if (size < 0 || size > limit - output.size()) {
                throw new IOException("HTTPS response exceeds the poster size limit");
            }
            if (size == 0) {
                int trailers = 0;
                while (true) {
                    String trailer = readLine(input, MAX_HEADER_LINE);
                    trailers += trailer.length() + 2;
                    if (trailers > MAX_HEADER_BYTES) throw new IOException("HTTPS trailers are too large");
                    if (trailer.isEmpty()) break;
                }
                return output.toByteArray();
            }
            byte[] chunk = readExact(input, (int) size);
            output.write(chunk);
            if (input.read() != '\r' || input.read() != '\n') throw new IOException("invalid HTTPS chunk terminator");
        }
    }

    private void writeResponse(DataOutputStream output, Result result) throws IOException {
        byte[] type = bytes(result.contentType, 8192);
        byte[] location = bytes(result.location, 8192);
        byte[] error = bytes(result.error, 8192);
        byte[] body = result.body == null ? new byte[0] : result.body;
        if (body.length > MAX_RESPONSE) {
            body = new byte[0];
            error = bytes("HTTPS response exceeds the poster size limit", 8192);
        }
        output.write(RESPONSE_MAGIC);
        output.writeInt(result.status);
        output.writeInt(body.length);
        output.writeShort(type.length);
        output.writeShort(location.length);
        output.writeShort(error.length);
        output.write(type);
        output.write(location);
        output.write(error);
        output.write(body);
        output.flush();
    }

    private static byte[] bytes(String value, int limit) {
        if (value == null) value = "";
        byte[] bytes = value.getBytes(StandardCharsets.UTF_8);
        if (bytes.length > limit) {
            byte[] shortened = new byte[limit];
            System.arraycopy(bytes, 0, shortened, 0, limit);
            return shortened;
        }
        return bytes;
    }

    @Override
    public synchronized void close() {
        running = false;
        if (listener != null) {
            try { listener.close(); } catch (IOException ignored) { }
            listener = null;
        }
        workers.shutdownNow();
        try { workers.awaitTermination(1, TimeUnit.SECONDS); } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
        }
        if (acceptThread != null) {
            try { acceptThread.join(1000); } catch (InterruptedException e) {
                Thread.currentThread().interrupt();
            }
            acceptThread = null;
        }
    }
}
