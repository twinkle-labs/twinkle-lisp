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

(defmacro (let* bindings &rest body)
  (if (null? bindings) 
      `(let () ,@body)
    `(let (,(car bindings))  (let* ,(cdr bindings) ,@body))))

(define (cadr x) (car (cdr x)))
(define (cdar x) (cdr (car x)))
(define (caar x) (car (car x)))
(define (cddr x) (cdr (cdr x)))
(define (caddr x) (car (cddr x)))
(define (cdddr x) (cdr (cddr x)))
(define (cadddr x) (car (cdddr x)))

(define (zero? x) (= x 0))
(define (notnull? x) (not (null? x)))
(define (max x y) (if (> x y) x y))
(define (min x y) (if (> x y) y x))

(defmacro (when test &rest body)
  `(if ,test (begin ,@body)))

(defmacro (add-to-list! l x)
  `(set! ,l (cons ,x ,l)))

(define (member x l)
  (cond
   ((null? l) false)
   ((eq? x (car l)) l)
   (else (member x (cdr l)))))

(define (reverse l)
  (let loop ((u l) (v ()))
    (if (null? u)
        v
        (loop (cdr u) (cons (car u) v)))))

(define (append l a)
  (let loop ((l (reverse l)) (r a))
       (if (null? l)
	   r
	 (loop (cdr l) (cons (car l) r)))))

(define (alist-set l key val)
 (let loop [(u l) (v ())]
  (cond [(null? u) (cons (cons key val) v)]
        [(eq? (car (car u)) key) (loop (cdr u) v)]
        [else (loop (cdr u) (cons (car u) v))])))

(define (mapcar l)
  (reverse
   (let loop ((l l) (r ()))
	(if (null? l)
	    r
	  (loop (cdr l) (cons (car (car l)) r))))))

(define (mapcdr l)
  (reverse
   (let loop ((l l) (r ()))
	(if (null? l)
	    r
	  (loop (cdr l) (cons (cdr (car l)) r))))))

(define (map fn &rest l)
  (reverse
   (let loop ((l l) (r ()))
	(if (or (null? l) (null? (car l)))
	    r
	  (loop (mapcdr l) (cons (apply fn (mapcar l)) r))))))

(define (for-each fn &rest l)
  (let loop ((l l))
       (if (not (or (null? l) (null? (car l))))
	   (begin
	    (apply fn (mapcar l))
	    (loop (mapcdr l))))))

(defmacro (for args &rest body)
  `(for-each (lambda ,(mapcar args) ,@body)
    ,@(mapcar (mapcdr args))))

(defmacro (dolist arg &rest body)
  `(for-each (lambda (,(car arg)) ,@body)
    ,(cadr arg)))

(define (random-member l)
  (let loop ((l l) (r false) (n 0))
       (if (null? l) (return r))
       (if (<= (random) (/ 1 (+ n 1)))
	   (loop (cdr l) l (+ n 1))
	 (loop (cdr l) r (+ n 1)))))

(define (filter pred l)
  (let loop ((u l) (v ()))
    (if (null? u) 
      (reverse v)
      (if (pred (car u))
        (loop (cdr u) (cons (car u) v))
        (loop (cdr u) v)))))

(define (remove pred l)
  (let loop ((u l) (v ()))
    (if (null? u) 
      (reverse v)
      (if (pred (car u))
        (loop (cdr u) v)
        (loop (cdr u) (cons (car u) v))))))

(define (merge-sort order l)
  (if (null? (cdr l)) 
    l
    (let loop ((u ()) (v ()) (l l))
      (if (null? l)
        (let merge ((u (merge-sort order u)) (v (merge-sort order v)) (r ()))
          (cond 
            ((null? u) (append (reverse r) v))
            ((null? v) (append (reverse r) u))
            ((order (car u) (car v)) (merge (cdr u) v (cons (car u) r)))
            (else (merge u (cdr v) (cons (car v) r)))))
        (loop v (cons (car l) u) (cdr l))))))

(defmacro (letrec bindings &rest body)
    (let ((vars (map car bindings))
	  (vals (map cadr bindings)))
    `(let ,(map (lambda (var) `(,var undefined)) vars)
       ,@(map (lambda (var val) `(set! ,var ,val)) vars vals)
       . ,body)))


(defmacro (delay expr)
  `(let ((value)) 
    (lambda(&label *promise*) 
      (if (eq? undefined value)
        (set! value ,expr))
      value)))

(define (force p) (p))

(defmacro (when pred &rest body)
  `(if ,pred 
    (begin ,@body)))

(defmacro (while pred &rest body)
  `(let loop () (when ,pred ,@body (loop))))

(define (alist-get l k)
  (define a (assoc k l))
  (if a (cdr a)))

(define (plist-get l k)
  (if (null? l) ()
    (if (eq? (car l) k) (cadr l)
      (plist-get (cdr l) k))))

(define (write-to-string obj)
  (buffer->string (with-output (open-output-buffer)
               (write obj)
               (get-output-buffer (current-output)))))
;; Dup
(define (stringify obj)
    (buffer->string (with-output (open-output-buffer)
               (write obj)
               (get-output-buffer (current-output)))))

;; Process elements of L one after another asynchronously.
;; FN is an asynchronous procedure.
;; FN is in the form of (fn e did). E is the element to be processed,
;; and when FN finish its work, it should call DID with the result x.
;; When all elements are processed, done will be invoked with the resulted
;; list.
(define (dolista fn l done)
  (let loop [(u l) (r ())]
       (if (null? u)
           (done (reverse r))
         (fn (car u) ^{[x] (loop (cdr u) (cons x r))}))))

(define (dolistq maxq fn l done)
  (let [(q 0) (u l) (r ())]
       (let loop ()
            (cond
             [(and (= q 0) (null? u)) (done r)]
             [(and (< q maxq) (not (null? u)))
             (let [(x (car u))]
                  (set! u (cdr u))
                  (set! q (+ q 1))
                  (fn x ^{[y]
                      (set! q (- q 1))
                      (set! r (cons (cons x y) r))
                      (loop)
                      }))]))))

                      
                      
(define (read-bytes input-port size)
  (define b (make-buffer size))
  (define a (open-output-buffer b))
  (pump input-port a size)
  b
  )


(define (read-from-file path)
  (define f (open-input-file path))
  (define x (read f))
  (close f)
  x)

(define (write-to-file data path)
  (define f (open-output-file path))
  (write data f)
  (close f)
  )


(define (pprint-indent i)
  (define sp "                             ")
  (if (<= i (string-length sp))
      (print (substring sp 0 i))
      (print sp)))

(define (pprint-one-liner? x)
  (let loop [(u x) (n 0)]
    (if (null? u)
	(< n 4)
	(if (pair? (car u))
	    false
	    (if (pair? (cdr u))
		(loop (cdr u) (+ n 1))
		(< n 4))))))

(define (pprint1 x i)
  (cond
   [(and (pair? x)
	 (not (pprint-one-liner? x)))
    (print "(")
    (pprint1 (car x) (+ i 1))
    (let loop [(u (cdr x))]
      (cond [(null? u)
	     (print ")")]
	    [(pair? u)
	     (println)
	     (pprint-indent (+ i 1))
	     (pprint1 (car u) (+ i 2))
	     (loop (cdr u))]
	    [else
	     (print " . ")
	     (pprint1 u (+ i 3))
	     (print ")")]))
    ]
   [else
    (write x)]))

(define (pprint x)
  (pprint1 x 0))

(define (pprintln x)
  (pprint1 x 0)
  (println))

;; Return the first n items in list l
(define (truncate-list l n)
  (let loop [(n n) (u l) (v ())]
    (if (or (<= n 0) (null? u))
	(reverse v)
	(loop (- n 1) (cdr u) (cons (car u) v)))))

;; x is an ENV object and ex is a procedure without arguments
;; ex's body will be evaluated inside x
(define (apply-extension x ex)
	(eval `(begin ,@(cdr (cdr ex))) x))

(define (error? x)
  (and (pair? x) (eq? (car x) 'error)))

(define (suffix? s suffix)
	(define i (string-find-backward s suffix))
	(if i (= i (- (length s) (length suffix))) false))

(define (prefix? s prefix)
  (eq? (string-find s prefix) 0))
