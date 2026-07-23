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

(with-temp-buffer
  (insert "ab")
  (goto-char 1)
  (condition-case nil (forward-char 9) (error nil))
  (km-emit "forward-char-boundary"))

(with-temp-buffer
  (insert "a b")
  (goto-char 1)
  (forward-word 9)
  (km-emit "forward-word-boundary"))

(with-temp-buffer
  (insert "a\n\nb")
  (goto-char 1)
  (forward-paragraph 9)
  (km-emit "forward-paragraph-boundary"))

(with-temp-buffer
  (insert "One.  Two.")
  (goto-char 1)
  (condition-case nil (forward-sentence 9) (error nil))
  (km-emit "forward-sentence-boundary"))

(with-temp-buffer
  (insert "One.  Two.")
  (goto-char (point-max))
  (backward-sentence 9)
  (km-emit "backward-sentence-boundary"))

(with-temp-buffer
  (transient-mark-mode 1)
  (insert "abcdef\nghijkl\nmnopqr")
  (goto-char 2)
  (rectangle-mark-mode 1)
  (goto-char 20)
  (kill-rectangle (region-beginning) (region-end))
  (goto-char 1)
  (yank-rectangle)
  (km-emit "rectangle"))
