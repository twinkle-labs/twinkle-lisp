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

(define (make-timer-queue &optional clk)
  ;; Sorted by run-time.
  ;; ( (<run-time> . <fn>) ...)
  (define q ())

  (defmethod (stat)
    (list (cons 'queue (length q))))
  
  (define (now)
    (if (procedure? clk) (clk) (microtime)))

  (define (insert t fn)
    (define item (cons t fn))
    (let loop ((q q) (p ()))
	 (cond ((null? q) (append (reverse p) (cons item ())))
	       ((< t (caar q)) (append (reverse p) (cons item q)))
	       (else (loop (cdr q) (cons (car q) p))))))

  (defmethod (add interval fn)
    (set! q (insert (+ (now) interval) fn))
    )

  (defmethod (del fn)
    (set! q (remove (lambda (item) (eq? (cdr item) fn)) q)))

  (defmethod (run-loop)
    (let loop ((t (run)))
	 (cond ((> t 0) (sleep t) (loop (run))))))

  (defmethod (run)
    (let loop ()
   (if (null? q) (return 0))
   (define item (car q))
   (define interval (- (car item) (now)))
   (cond ((<= interval 0)
    (set! q (cdr q))
    ((cdr item))
    (loop))
         (else interval))))

  (this))


(define (test-timer)
  (define tq (make-timer-queue))
  (define n 0)
  (define (t1)
    (set! n (+ n 1))
    (println (date) ": t1: " n)
    (tq 'add 2 t1)
    (if (> n 10) (tq 'del t2))
    )
  (define (t2)
    (set! n (+ n 1))
    (println (date) ": t2: " n)
    (tq 'add 4 t2)
    )

  (tq 'add 0 t1)
  (tq 'add 1 t2)

  (tq 'run-loop)
  )


		 
