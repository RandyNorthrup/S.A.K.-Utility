import re

FILE = r'c:\Users\Randy\Coding\S.A.K.-Utility\src\gui\wifi_qr_panel.cpp'
text = open(FILE, encoding='utf-8').read()

needles = ['m_preview_timer', 'centerImagePath', 'onFieldChanged', 'refreshPreview', 'onExportPngClicked', 'onBatchExportClicked']
for n in needles:
    idx = text.find(n)
    if idx >= 0:
        print(f"FOUND '{n}' at {idx}:")
        print(repr(text[max(0,idx-40):idx+80]))
        print()
    else:
        print(f"NOT FOUND: {n}")
