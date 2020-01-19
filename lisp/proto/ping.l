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

;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
;; ping.l -- A simple ping protocol
;;
;; Client sends 10 ping messages to the server then
;; shutdown gracefully.
;;
;; Protocol Arguments
;; in
;; out
;; args
;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;

(define count 10)

(defmethod (run)
    (cond
     [(ready? in)
      (let [(x (read in))]
	   (println "Got:" x)
	   (match x
		  [(ping t)
		   (write `(pong ,t ,(microtime)) out)]
		  [(pong x y)
		   (println "Pong: " (- y x) " " (- (microtime) y) " total:" (- (microtime) x))
		   (set! count (- count 1))
		   (if (> count 0)
		       (write `(ping ,(microtime)) out)
		       (write `(bye) out)
		       )
		   ]
		  [(bye)
		   (exit)
		   false]
		  [(keep-alive)]
		  [else
		   (if (eq? x *eof*)
		       (begin (exit) false)
		       (error "Unsupported message"))
		   ]))]
     [else false]
     ))

;; Client Only
(defmethod (ready)
  (write `(ping ,(microtime)) out)
  (flush out)
  )
