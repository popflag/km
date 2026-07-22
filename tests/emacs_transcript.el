;;; emacs_transcript.el --- Canonical editor transcript  -*- lexical-binding: t; -*-
;;; Positions are GNU Emacs 1-based characters.

(defun km-emit (name)
  (let ((bytes (encode-coding-string (buffer-string) 'utf-8-unix)))
    (princ name)
    (princ ":")
    (dotimes (i (length bytes))
      (princ (format "%02x" (aref bytes i))))
    (princ (format ":%d\n" (point)))))

(with-temp-buffer
  (insert "éCOLE foo")
  (goto-char 1)
  (capitalize-word 1)
  (goto-char 7)
  (upcase-word 1)
  (km-emit "case"))

(with-temp-buffer
  (insert "a \t b")
  (goto-char 4)
  (just-one-space)
  (km-emit "space"))

(with-temp-buffer
  (insert "aa\n  bb")
  (goto-char 5)
  (delete-indentation)
  (km-emit "join"))

(with-temp-buffer
  (insert "a\n\n\nb")
  (goto-char 3)
  (delete-blank-lines)
  (km-emit "blank"))

(with-temp-buffer
  (insert "aa\nbb\ncc\ndd")
  (goto-char 5)
  (transpose-lines 2)
  (km-emit "transpose"))

(with-temp-buffer
  (insert "A中B")
  (goto-char 3)
  (km-emit "goto"))

(with-temp-buffer
  (insert "a\nb")
  (goto-char 3)
  (transpose-lines 1)
  (km-emit "transpose-final"))

(with-temp-buffer
  (insert "a\nb")
  (goto-char 3)
  (transpose-lines 2)
  (km-emit "transpose-final-2"))
