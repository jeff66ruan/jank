(ns jank.type.declaration
  (:use clojure.walk
        clojure.pprint
        jank.assert))

(defn shorten-types [item]
  "Walks through the decl and replaces all [:type ...] instances with
   their shorter type names. Example: [:type [:identifier \"string\"]]
   becomes (\"string\")"
  (postwalk
    (fn [x]
      (if (and (vector? x) (= :type (first x)))
        (rest (second x))
        x))
    item))

(defn function? [decl-type]
  "Returns whether or not the provided type is that of a function."
  (= "ƒ" (first decl-type)))

(defn auto? [decl-type]
  "Returns whether or not the provided type is to be deduced."
  (or (= "∀" (first decl-type)) (= "auto" (first decl-type))))

(defn lookup-overloads [decl-name scope]
  "Recursively looks through the hierarchy of scopes for the declaration.
   Returns all overloads in all scopes, from closest to furthest."
  (loop [current-scope scope
         overloads []]
    (if current-scope
      (if-let [found (find (:binding-declarations current-scope) decl-name)]
        (recur (:parent current-scope) (into overloads (second found)))
        (recur (:parent current-scope) overloads))
      overloads)))

(defn lookup [decl-name scope]
  "Recursively looks through the hierarchy of scopes for the declaration.
   Returns the first set of overloads found in the closest scope, not all.
   See lookup-overloads for getting all."
  (loop [current-scope scope]
    (when current-scope
      (if-let [found (find (:binding-declarations current-scope) decl-name)]
        found
        (recur (:parent current-scope))))))

(defn validate [decl-name decl-type scope]
  "Looks up a declaration, if any, and verifies that the provided
   declaration has a matching type. Returns the decl or nil, if none is found."
  (let [decl (lookup decl-name scope)
        wrapped-type {:type decl-type}]
    (when (some? decl)
      (let [expected-types (second decl)]
        ; All binding declarations must be the same type unless they're for
        ; function overloads. In that case, all declarations must be functions.
        ; The only exception is matching an auto declaration against a complete
        ; type.
        (type-assert (or (some #(= wrapped-type %) expected-types)
                         (some (comp auto? :type) expected-types)
                         (and (function? (:type wrapped-type))
                              (every? (comp function? :type) expected-types)))
                     (str "declaration of "
                          decl-name
                          " as "
                          wrapped-type
                          " doesn't match previous declarations "
                          expected-types))))
    decl))

(defmulti lookup-type
  "Recursively looks through the hierarchy of scopes for the declaration.
   Expects the *shortened* type. See shorten-types."
  (fn [decl-type scope]
    (let [name (first decl-type)]
      (cond
        (or (= "ƒ" name) (= "function" name))
        :function
        (or (= "∀" name) (= "auto" name))
        :auto
        :else
        :default))))

(defmethod lookup-type :function [decl-type scope]
  ; Function types always "exist" as long as they're well-formed
  (let [generics (second decl-type)]
    (type-assert (= (count generics) 3) "invalid function type format")
    (when (> (count (second generics)) 1)
      (type-assert (some? (lookup-type (second (second generics)) scope))
                   "invalid function parameter type"))
    (when (> (count (nth generics 2)) 1)
      (type-assert (some? (lookup-type (second (nth generics 2)) scope))
                   "invalid function return type"))
    decl-type))

(defmethod lookup-type :auto [decl-type scope]
  (list "auto"))

(defmethod lookup-type :default [decl-type scope]
  "Recursively looks up a type by name. Expects the *shortened* type.
   Returns the type, if found, or nil."
  (loop [current-scope scope]
    ; TODO: Handle generic types properly
    (when current-scope
      (if-let [found ((:type-declarations current-scope) decl-type)]
        found
        (recur (:parent current-scope))))))

(defmulti add-to-scope
  (fn [item scope]
    (let [kind (first (second item))]
      (cond
        (= :type kind)
        :type-declaration
        (= :identifier kind)
        :binding-declaration
        (= :implicit-declaration kind)
        :implicit-declaration
        :else
        (type-assert false (str "invalid binding " item))))))

(defmethod add-to-scope :type-declaration [item scope]
  "Adds the opaque type declaration to the scope.
   Returns the updated scope."
  (let [decl-name (first (shorten-types (rest item)))]
    (update scope :type-declarations conj decl-name)))

(defmethod add-to-scope :binding-declaration [item scope]
  "Finds, validates, and adds the provided declaration into the scope.
   Returns the updated scope."
  (let [shortened (shorten-types item)
        decl-name (get-in shortened [1 1])
        decl-type (get-in shortened [2])
        found-decl (validate decl-name decl-type scope)
        found-type (lookup-type decl-type scope)]
    (type-assert (some? found-type) (str "unknown type " decl-type))

    (cond
      ; If we're seeing this binding for the first time
      (nil? found-decl)
      (update scope :binding-declarations assoc decl-name [{:type decl-type}])

      ; If we're adding an overload
      (and (= -1 (.indexOf (second found-decl) {:type decl-type}))
           (function? decl-type))
      ; First remove any matching overloads with auto return types. This allows
      ; defined functions to replace previous declarations where the return
      ; type wasn't yet deduced.
      (let [without-auto (update-in scope
                                    [:binding-declarations decl-name]
                                    (partial
                                      remove
                                      #(= (second (second decl-type))
                                          (second (second (:type %))))))]
        (update-in without-auto
                   [:binding-declarations decl-name]
                   conj
                   {:type decl-type}))

      ; Multiple declaration; nothing changes
      :else
      scope)))
