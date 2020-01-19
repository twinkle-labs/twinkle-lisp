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

(define (make-request-queue)
  (define request-list ())
  (define self-pid (get-pid))

  ;; Outstanding requests older than 10s will be removed
  ;; from the queue so even if the response come in future
  ;; it will not be handled.
  ;; TODO we should call callback with false when timeout?
  (defmethod (add-request req-id req-time callback)
    (define req (list req-id req-time callback))
    (define expire-time (- (time) 10))
    (let loop [(u request-list) (v ())]
         (cond
          [(null? u)
          (set! request-list (cons req v))]
          [(< (cadr (car u)) expire-time)
          (loop (cdr u) v)]
          [else (loop (cdr u) (cons (car u) v))])))

  ;; Send asynchronous request to process
  ;; and return the request id
  ;; The response will be sent back by a
  ;; (did-request req-id response)
  (defmethod (send pid message done)
    (define req-id (random))
    (if (send-message pid (list 'request self-pid req-id message))
        (begin
         (println "request sent to " pid ":" message)
         (add-request req-id (time) done))
      (println "request-queue: faild to send to pid=\{pid}, message=\{message}")))

  (defmethod (did-request req-id response)
    (define req (assoc req-id request-list))
    (if req
        (apply (caddr req) (list response))))

  (this))
