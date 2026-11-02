"""Reads an MBOX file with Python's standard mailbox module and prints what it found, as JSON.

An independent reader for test_mbox_python_reader: the app wrote the file and its own parser wrote
the matching assertions for years, so only something else reading the same bytes can say the file
is really an MBOX that other programs understand.
"""

import email.header
import email.utils
import hashlib
import json
import mailbox
import sys


def decoded(value):
    """A header value in plain text, whichever encoded-word form the writer chose."""
    if value is None:
        return None
    parts = []
    for chunk, charset in email.header.decode_header(str(value)):
        if isinstance(chunk, bytes):
            parts.append(chunk.decode(charset or "ascii", "replace"))
        else:
            parts.append(chunk)
    return "".join(parts)


def describe(message):
    """The fields the test compares, for one message."""
    attachments = []
    body = None
    for part in message.walk():
        if part.is_multipart():
            continue
        name = part.get_filename()
        payload = part.get_payload(decode=True) or b""
        if name is not None:
            attachments.append(
                {
                    "name": decoded(name),
                    "size": len(payload),
                    "sha256": hashlib.sha256(payload).hexdigest(),
                }
            )
        elif part.get_content_type() == "text/plain" and body is None:
            body = payload.decode(part.get_content_charset() or "utf-8", "replace")
    date = message["Date"]
    parsed = email.utils.parsedate_to_datetime(date) if date else None
    return {
        "subject": decoded(message["Subject"]),
        "from": decoded(message["From"]),
        "to": decoded(message["To"]),
        "date_utc": parsed.timestamp() if parsed is not None else None,
        "has_separator": message.get_from() is not None,
        "body": body,
        "attachments": attachments,
    }


def main():
    box = mailbox.mbox(sys.argv[1], create=False)
    messages = [describe(box[key]) for key in box.keys()]
    box.close()
    json.dump({"count": len(messages), "messages": messages}, sys.stdout)


if __name__ == "__main__":
    main()
