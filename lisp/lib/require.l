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

(define require-alist ())

(define (require-load filename)
  (let ((p (find-file filename)))
    (if (not p) (error "Required file missing: " filename))
    (define x (assoc p require-alist))
    (if x (return (cdr x)))
    (set! require-alist (cons (cons p ((lambda ()
     	(load filename)
    ))) require-alist))
    (cdr (car require-alist))))

(define (require-import env imports exports)
	(for-each 
		(lambda (a)
			(define x (if (symbol? a) a (car a)))
			(define y (if (symbol? a) a (cadr a)))
			(define binding (assoc x exports))
			(if binding
				(eval `(define ,y ,(cdr binding)) env)
				(error "Error import: " x)))
		imports))

;; require loads a file once and return the previous loaded result
;; on subsequent loads.
;; require accepts symbols to be imported. In this case
;; the result must be an assocation list of the form ((x . 1) (y . 2))
;; nad then those sym bols will be injected into the environment where
;; require is called.
(defmacro (require filename &rest imports)
	`(let ((super (this)) (exports (require-load (eval ,filename))))
		(require-import super ',imports exports)
		exports
	))
