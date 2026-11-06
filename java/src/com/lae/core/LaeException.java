// SPDX-FileCopyrightText: 2026 mtq0605
// SPDX-License-Identifier: Apache-2.0
package com.lae.core;

/** Every failure of the LAE codecs (the C++ exit(), throw and error returns), with the C++ message text. */
public class LaeException extends RuntimeException {
    private static final long serialVersionUID = 1L;
    public LaeException(String message) { super(message); }
    public LaeException(String message, Throwable cause) { super(message, cause); }
}
