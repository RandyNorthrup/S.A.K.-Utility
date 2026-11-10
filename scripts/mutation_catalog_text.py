"""Bind LF-authored mutation catalogs to source bytes without changing source EOLs."""


def adapt_mutation_text(source: str, find: str, replacement: str) -> tuple[str, str]:
    """Return needles in the source's EOL style; refuse ambiguous mixed input.

    Catalog JSON uses LF so one entry works on either checkout style. The
    mutation runner still writes and restores the original source bytes.
    """
    if not isinstance(find, str) or not isinstance(replacement, str):
        raise ValueError("mutation find and replace must be strings")

    without_crlf = source.replace("\r\n", "")
    if "\r" in without_crlf or ("\r\n" in source and "\n" in without_crlf):
        raise ValueError("mutation source has mixed or bare-CR line endings")

    def catalog_lf(value: str) -> str:
        canonical = value.replace("\r\n", "\n")
        if "\r" in canonical:
            raise ValueError("mutation catalog has bare-CR line endings")
        return canonical

    newline = "\r\n" if "\r\n" in source else "\n"
    return (catalog_lf(find).replace("\n", newline),
            catalog_lf(replacement).replace("\n", newline))
