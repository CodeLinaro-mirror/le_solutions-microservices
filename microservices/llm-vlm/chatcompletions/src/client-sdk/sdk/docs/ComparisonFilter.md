# ComparisonFilter

A filter used to compare a specified attribute key to a given value using a defined comparison operation. 

## Properties

Name | Type | Description | Notes
------------ | ------------- | ------------- | -------------
**type** | **str** | Specifies the comparison operator: &#x60;eq&#x60;, &#x60;ne&#x60;, &#x60;gt&#x60;, &#x60;gte&#x60;, &#x60;lt&#x60;, &#x60;lte&#x60;. - &#x60;eq&#x60;: equals - &#x60;ne&#x60;: not equal - &#x60;gt&#x60;: greater than - &#x60;gte&#x60;: greater than or equal - &#x60;lt&#x60;: less than - &#x60;lte&#x60;: less than or equal  | [default to 'eq']
**key** | **str** | The key to compare against the value. | 
**value** | [**ComparisonFilterValue**](ComparisonFilterValue.md) |  | 

## Example

```python
from openapi_client.models.comparison_filter import ComparisonFilter

# TODO update the JSON string below
json = "{}"
# create an instance of ComparisonFilter from a JSON string
comparison_filter_instance = ComparisonFilter.from_json(json)
# print the JSON string representation of the object
print(ComparisonFilter.to_json())

# convert the object into a dict
comparison_filter_dict = comparison_filter_instance.to_dict()
# create an instance of ComparisonFilter from a dict
comparison_filter_from_dict = ComparisonFilter.from_dict(comparison_filter_dict)
```
[[Back to Model list]](../README.md#documentation-for-models) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to README]](../README.md)


