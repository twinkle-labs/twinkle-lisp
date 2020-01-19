;;    
;; Copyright (C) 2020, Twinkle Labs, LLC.
;;
;; This program is free software: you can redistribute it and/or modify
;; it under the terms of the GNU Affero General Public License as published
;; by the Free Software Foundation, either version 3 of the License, or
;; (at your option) any later version.
;;
;; This program is distributed in the hope that it will be useful,
;; but WITHOUT ANY WARRANTY; without even the implied warranty of
;; MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
;; GNU Affero General Public License for more details.
;;
;; You should have received a copy of the GNU Affero General Public License
;; along with this program.  If not, see <https://www.gnu.org/licenses/>.
;;

;; -*- mode: scheme -*-

;; These are common code that is only usable in TWK project

(define (pubkey->address pk) 
  (base58-check-encode 0 (rmd160 (sha256 pk)))
  )


(define (find-blob-refs s)
  (define blob-pat (regexp-compile "blob/([a-fA-F0-9]{64,64})"))
  (let loop [(pos 0)(r ())]
    (define u (regexp-match blob-pat s pos))
    (if u
	(loop (+ (caar u) (cdar u))
	      (cons (substring s
			       (car (cadr u))
			       (+ (car (cadr u))
				  (cdr (cadr u))))
		    r))
	r)))

(define (find-mention-refs s)
  (define pat (regexp-compile "@([a-zA-Z0-9]{4,35})"))
  (let loop [(pos 0)(r ())]
    (define u (regexp-match pat s pos))
    (if u
	(loop (+ (caar u) (cdar u))
	      (cons (substring s
			       (car (cadr u))
			       (+ (car (cadr u))
				  (cdr (cadr u))))
		    r))
	r)))



(define (message-digest msg)
  (if (list? msg)
      (set! msg (stringify msg))
    (if (not (string? msg))
        (error "Invalid Message")))
  (base58-encode (sha256 msg))
  )

(define (digest-id data)
  (println "digest-id is deprecated, use message-digest instead")
  (base58-encode (sha256 data))
  )


(defmacro (load-module filename)
  `((lambda ()
    (load ,filename)
    (this)
    )))

(define (blob-hash->path x)
  ;; Convert a blob hash x to file system path in the form of
  ;;   .../blob/<xx>/<yyyyyyyyyyyy...>
  
  "\{*var-path*}/data/blob/\{(slice x 0 2)}/\{(slice x 2)}"
  )

(define (ip-address? x)
  (regexp-match "^\\d+\\.\\d+.\\d+.\\d+$" x))

(define (get-ipv4-from-addr-info u)
  (if (null? u)
      false
      (if (eq? (get (car u) 'family) 'inet)
	  (get (car u) 'ip)
	  (get-ipv4-from-addr-info (cdr u)))))

(define (get-ipv6-from-addr-info u)
  (if (null? u)
      false
      (if (eq? (get (car u) 'family) 'inet6)
	  (get (car u) 'ip)
	  (get-ipv6-from-addr-info (cdr u)))))

