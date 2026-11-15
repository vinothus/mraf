package com.mraf.server.mcp;

/**
 * Raised inside MCP tool execution; carries a JSON-RPC error code.
 */
public class McpException extends RuntimeException {
    private final int code;

    public McpException(int code, String message) {
        super(message);
        this.code = code;
    }

    public int getCode() {
        return code;
    }
}
