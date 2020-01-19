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
;; sqlite3.l -- SQLite3 API Utilities
;;
;; Provides:
;;  - open-sqlite3-database: simpler to work with than low level API
;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;

(define (open-sqlite3-database path)
  ;; Provide a sqlite3 database instance that simplifies data access.
  ;; Common operations, like insert/find/update/remove, are provided
  ;; to avoid writing inline SQLs by hand.
  ;; Mixing SQL with LISP looks pretty bad,
  ;; and it's a nightmare to put query parameters into correct order,
  ;; especially when there are so many of them.
  ;;
  ;; Convention:
  ;; 1. field name must be a valid symbol;
  ;; 2. tables must have ROWID (by default);
  ;;
  ;; Simple use comes with a great performance penalty,
  ;; which is usually acceptable. However,
  ;; if performance is important or advanced data manipulation is required,
  ;; we can use first/query to implement custom queries.
  ;; For even better performance, consider using (prepare) method,
  ;; which returns a procedure that can execute query with a precompiled
  ;; statement instance. 
  ;;
  ;; Most of the time, stop worrying and use the simple ones.
  ;; We can always optimze later.
  ;; 
  ;; Any query failure will trigger errors.
  ;; the caller may need to use catch to protect.
  ;; TODO
  ;; - provide error reasons
  ;; - measure query performance and log heavy ones
  
  (define db (sqlite3-open path))

  (define (dup n x)
    ;; Duplicate <x> into a list for <n> times.
    (let loop [(n n) (r ())]
      (if (= n 0)
          r
          (loop (- n 1) (cons x r)))))
  
  (defmethod (insert table-name &rest attrs)
    ;; Insert a row into table.
    ;; Return the inserted row id, which is also the value of integer primary key
    ;; if such a field is defined on the table.
    ;; Example:
    ;;   (<db> 'insert <table-name> :name "Alice" :age 10)

    (if (not (string? table-name))
        (error "Missing table name"))

    (define fields (join (mapcar attrs) ","))
    (define x (join (dup (length attrs) "?") ","))    
    (define sql "INSERT INTO \{table-name} (\{fields}) VALUES (\{x})")
    (apply query (cons sql (mapcdr attrs)))
    (sqlite3-last-insert-rowid db))

  (defmethod (insert-or-update table-name &rest attrs)
    ;; Try to insert a row, but if a unique conflict happens,
    ;; update existing row with new data.
    ;; The first attr must be declared as unique on the table.
    ;; Return the id of the row that is either created or updated.
    (if (not (string? table-name))
        (error "Missing table name"))

    (define fields (join (mapcar attrs) ","))
    (define x (join (dup (length attrs) "?") ","))
    (define k (caar attrs))
    ;; TODO filter out ctime from attrs
    (define u (join (map ^{[x](concat (car x) "=excluded." (car x))}
			 (cdr attrs)) ","))
    
    (define sql "INSERT INTO \{table-name} (\{fields}) VALUES (\{x}) 
ON CONFLICT(\{k}) DO UPDATE SET \{u};")
    (apply query (cons sql (mapcdr attrs)))
    (cdar (first "SELECT rowid FROM \{table-name} WHERE \{k}=?" (cdar attrs))))
  
  ;;------------------------------------------------------------

  (define (find-helper table-name attrs fn)
    ;;
    ;; (find <table-name> :id 1 :select "id,name AS Name")
    ;; (find <table-name> :where (list "id=?" 1))
    ;;

    (if (not (string? table-name))
        (error "Missing table name"))

    (define select ())
    (define where ())
    (define params ())

    (dolist (a attrs)
      (cond 
        [(eq? (car a) 'select)
          (set! select (cons (cdr a) select))
        ]
        [(eq? (car a) 'where)
          (set! where (cons (cadr a) where))
          (set! params (append (cddr a) params))
        ]
        [else
          ;; This is a field
          (set! where (cons (concat (car a) "=?") where))
          (set! params (cons (cdr a) params))
        ]))

    (if (null? select) 
      (set! select "*")
      (set! select (join select ",")))

    (if (null? where)
      (set! where "")
      (set! where (concat "WHERE " (join where " AND ")))
      )

    (define sql "SELECT \{select} FROM \{table-name} \{where}")
    (apply fn (cons sql params))
    )

  (defmethod (find table-name &rest attrs)
    ;; Find the first row that satisfies the condition
    ;; specified in attrs.
    ;; You can use :select to customize the fields you want, and also
    ;; rename the field if necessary. By default, all fields are returned.
    ;; Complex conditions can be specified with :where.
    ;; There can be multiple :where.
    
    (find-helper table-name attrs first))
  
  (defmethod (find-all table-name &rest attrs)
    ;; Similar to find, but it return all rows
    ;; that satisfy the contion in attrs.
    
    (find-helper table-name attrs query))

  ;;------------------------------------------------------------
  
  (defmethod (update table-name &rest attrs)
    ;; Update rows which satisfy the attrs condition.
    ;; If no :where is present, then the first attr
    ;; is used as the condition.
    ;; otherwise, only :where is used.
    
    (if (not (string? table-name))
        (error "Missing table name"))

    ;; if where is present, then use where 
    ;; assume the first attr is the primary key
    (define where ())
    (define params ())
    (define fields ())

    (dolist (x attrs)
      (cond
       [(eq? (car x) 'where)
        (set! where (cons (cadr x) where))
        (set! params (append (cddr x) params))
        ]
       [else
        (set! fields (cons x fields))
        ]))

    ;; If no :where is provided, then the first attribute
    ;; is used for lookup.
    (if (null? where)
        (begin
          (set! where (concat (caar attrs) "=?"))
          (set! params (list (cdr (car attrs))))
          (set! fields (cdr attrs))
          )
        (set! where (join where " AND ")))

    (define u (join (map ^{[x]"\{(car x)}=?"} fields) ","))
    (define sql "UPDATE \{table-name} SET \{u} WHERE \{where}")
    (apply first (cons sql (append (mapcdr fields) params)))
    )

  (define (make-condition attrs)
    ;; A helper function.
    ;; Return ("<conditions>" x y z ...).
    ;; If attrs is empty, return NIL.
    ;; shared among remove,count,has?
    (define where ())
    (define params ())

    (dolist (x attrs)
            (cond
             [(eq? (car x) 'where)
              (set! where (cons (cadr x) where))
              (set! params (append (cddr x) params))
              ]
             [else
              (set! where (cons (concat (car x) "=?") where))
              (set! params (cons (cdr x) params))
              ]))

    (if (null? where)
        ()
        (cons (join where " AND ") params)))

  ;;------------------------------------------------------------

  (defmethod (remove table-name &rest attrs)
    ;; Remove rows that satisfy the condition in attrs.
    ;; Condition is specified with value attributes or :where, or both.
    ;; Example:
    ;;   (db 'remove "user" :name "Alice")
    ;;   (db 'remove "user" :where (list "age>?" 30))
    ;; At lease one condition must be provided.
    ;; Use (remove-all) to remove all rows in a table.
    (if (not (string? table-name))
        (error "Missing table name"))

    (define condition (make-condition attrs))
    (if (null? condition)
        (error "No condition"))
    
    (apply query (cons "DELETE FROM \{table-name} WHERE \{(car condition)}"
                       (cdr condition))))

  ;;------------------------------------------------------------

  (defmethod (remove-all table-name)
    ;; Remove all rows in table.
    ;; Be careful.
    (sqlite3-exec "DELETE FROM \{table-name}"))
  
  ;;------------------------------------------------------------
  
  (defmethod (has? table-name &rest attrs)
    ;; Check if there is any row that satisfies the condition in attrs.
    ;; Condition is specified with value attributes or :where, or both.
    ;; Example:
    ;;   (db 'has? "user" :name "Alice")
    ;;   (db 'has? "user" :where (list "age>?" 30))
    ;; At lease one condition must be provided.
    ;; This is usually the better way to check if a record exists
    ;; than using (count), since it will stop at the first row.
    (if (not (string? table-name))
        (error "Missing table name"))
    (define condition (make-condition attrs))
    (if (null? condition)
        (error "No condition"))
    (= 1 (cdar (apply first (cons "SELECT EXISTS(SELECT 1 FROM \{table-name} WHERE \{(car condition)}) AS p"
                       (cdr condition))))))

  ;;------------------------------------------------------------
  (defmethod (count table-name &rest attrs)
    ;; Return the number of rows that satisfy the condition in attrs.
    ;; Condition is specified with value attributes or :where, or both.
    ;; Example:
    ;;   (db 'count "user" :role "admin")
    ;;   (db 'count "user" :where (list "age>?" 30))
    ;; If no condition is provided, return the count of all rows.
    (if (not (string? table-name))
        (error "Missing table name"))
    (define condition (make-condition attrs))
    (if (null? condition)
        (set! condition '("1")))
    (cdar (apply first (cons "SELECT COUNT(*) AS cnt FROM \{table-name} WHERE \{(car condition)}"
                       (cdr condition)))))
  
  ;;------------------------------------------------------------
  (defmethod (prepare sql &optional step)
    ;; Compile a SQL statement, return a procedure for execution.
    ;; If step is true, then the statement will be step once,
    ;; otherwise it will be run until it is done.
    ;;
    ;; For high performance queries,
    ;; SQL construction and compilation overhead is too much.
    ;; To avoid reconstruction and recompilation every run,
    ;; the statements is first prepared and then cached,
    ;; so that it can be used again and again.
    ;;
    ;; The procedure returned can be used as an ordinary procedure.
    (define stmt (sqlite3-prepare db sql))
    (lambda (&rest args)
      (define begin-time (begin-query "PREPARED" sql args))
      (sqlite3-reset stmt) ;; In case the stmt wasn't finished in last run
      (sqlite3-bind stmt args)
      (define r (if step (sqlite3-step stmt) (sqlite3-run stmt)))
      (sqlite3-reset stmt) ;; Mark the statement as finished
      (end-query r begin-time)
      (if r r (error "Prepared query" step))
      ))

  ;;------------------------------------------------------------
  (defmethod (first sql &rest args)
    ;; Run a query and return the first row of result.
    ;; Return the first result
    ;; Otherwise throw an error
    (define begin-time (begin-query "FIRST" sql args))
    (define stmt (sqlite3-prepare db sql))
    (sqlite3-bind stmt args)
    (define r (sqlite3-step stmt))
    (close stmt)
    (end-query r begin-time)
    (if r r (error "QUERY")))
  
  ;;------------------------------------------------------------
  (defmethod (query sql &rest args)
    ;; Run a query and return all rows in result set.
    ;; Return all result
    ;; Otherwise false.
    (define begin-time (begin-query "ALL" sql args))
    (define stmt (sqlite3-prepare db sql))
    (sqlite3-bind stmt args)
    (define all (sqlite3-run stmt))
    (close stmt)
    (end-query all begin-time)
    (if all all (error "QUERY"))
    )

  ;;------------------------------------------------------------
  (defmethod (open-blob-input table-name field rowid)
    (sqlite3-open-blob-input db "main" table-name field rowid)
    )

  (defmethod (open-blob-output table-name field rowid)
    (sqlite3-open-blob-output db "main" table-name field rowid)
    )
  
  (defmethod (exec sql)
    ;; Execute a query without parameters
    (define begin-time (begin-query "EXEC" sql ()))
    (define x (sqlite3-exec db sql))
    (end-query x begin-time)
    )

  (defmethod (last-insert-id)
    ;; Return last inserted row id
    (sqlite3-last-insert-rowid db))

  (defmethod (begin-transaction)
    ;; must be paired with either rollback or commit
    (vverbose "**** BEGIN TRANSACTION *****")
    ;; Use IMMEDIATE transaction
    ;; to avoid 'Database is locked' errors
    ;; By default, it's deferred.
    ;; Not suitable for multi writer
    (sqlite3-exec db "BEGIN IMMEDIATE TRANSACTION"))

  (defmethod (rollback)
    ;; 
    ;; Usually when error occurs
    (vverbose "!!!! ROLLBACK TRANSACTION !!!!")
    (sqlite3-exec db "ROLLBACK TRANSACTION"))

  (defmethod (commit)
    ;; end a transaction.
    ;; should pair with begin-transaction
    (vverbose "**** COMMIT TRANSACTION ****")
    (sqlite3-exec db "COMMIT TRANSACTION"))

  (define (begin-query tag sql args)
    (vverbose "QUERY: " tag ":" (join (split sql "\n") " "))
    (if (not (null? args))
	(vverbose "PARAMS: " args))
    (microtime))

  (define (end-query result begin-time)
    (define dur (- (microtime) begin-time))
    (vverbose " => " result)
    (if (> dur 0.0001)
       (vverbose "TIME: " dur))
    )

  (defmethod (finalize)
    (close db)
    )
  
  (this)
  ;; End of (open-sqlite3-database)
  )

;;!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!
;; Consider the following procedures obsolete
;;!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!

(define (sqlite3-query db sql &rest args)
  (define stmt (sqlite3-prepare db sql))
  (sqlite3-bind stmt args)
  (define all (sqlite3-run stmt))
  (close stmt)
  (if all all ()))

(define (sqlite3-first db sql &rest args)
  (define stmt (sqlite3-prepare db sql))
  (sqlite3-bind stmt args)
  (define r (sqlite3-step stmt))
  (close stmt)
  (if r r ()))
      

  
